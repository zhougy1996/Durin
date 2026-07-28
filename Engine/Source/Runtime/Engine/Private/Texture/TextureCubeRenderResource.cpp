#include "Texture/TextureCubeRenderResource.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	auto FTextureCubeResourceCompletion::BeginRequest(uint64 Revision) -> void
	{
		RequestedRevision.store(Revision, std::memory_order_release);
		FailedRevision.store(0, std::memory_order_release);
		FailureReason.store(
			ETextureRenderFailure::None, std::memory_order_release);
		SetResourceState(ERenderResourceState::Pending, Revision);
	}

	auto FTextureCubeResourceCompletion::MarkBuilding(uint64 Revision) -> bool
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return false;
		SetResourceState(ERenderResourceState::Building, Revision);
		return true;
	}

	auto FTextureCubeResourceCompletion::MarkReady(uint64 Revision) -> void
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return;
		AppliedRevision.store(Revision, std::memory_order_release);
		FailedRevision.store(0, std::memory_order_release);
		FailureReason.store(
			ETextureRenderFailure::None, std::memory_order_release);
		SetResourceState(ERenderResourceState::Ready, Revision);
	}

	auto FTextureCubeResourceCompletion::MarkFailed(
		uint64 Revision, ETextureRenderFailure Reason) -> void
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return;
		FailureReason.store(Reason, std::memory_order_release);
		FailedRevision.store(Revision, std::memory_order_release);
		SetResourceState(ERenderResourceState::Failed, Revision);
	}

	auto FTextureCubeResourceCompletion::MarkReleased(uint64 Revision) -> void
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return;
		AppliedRevision.store(Revision, std::memory_order_release);
		FailedRevision.store(0, std::memory_order_release);
		FailureReason.store(
			ETextureRenderFailure::None, std::memory_order_release);
		SetResourceState(ERenderResourceState::Released, Revision);
	}

	auto FTextureCubeResourceCompletion::GetResourceState() const
		-> ERenderResourceState
	{
		std::lock_guard Lock(ResourceStateMutex);
		if (ResourceStateRevision
			!= RequestedRevision.load(std::memory_order_acquire))
		{
			return ERenderResourceState::Pending;
		}
		return ResourceState;
	}

	auto FTextureCubeResourceCompletion::SetResourceState(
		ERenderResourceState State, uint64 Revision) -> void
	{
		std::lock_guard Lock(ResourceStateMutex);
		ResourceState = State;
		ResourceStateRevision = Revision;
	}

	FTextureCubeResource::FTextureCubeResource(
		FTextureReference* InTextureReference,
		std::shared_ptr<const FTextureCubePlatformData> InPlatformData,
		uint64 InRevision,
		std::shared_ptr<FTextureCubeResourceCompletion> InCompletion)
		: FTextureResource(InTextureReference)
		, PlatformData(std::move(InPlatformData))
		, Revision(InRevision)
		, Completion(std::move(InCompletion))
	{
		check(PlatformData && PlatformData->IsValid());
		check(Completion != nullptr);
	}

	FTextureCubeResource::~FTextureCubeResource() = default;

	auto FTextureCubeResource::InitRHI(FRHICommandListBase& RHICmdList) -> void
	{
		check(IsInRenderingThread());
		if (!Completion->MarkBuilding(Revision)) return;

		const FTexture2DMipData& BaseMip =
			PlatformData->Faces[0].Mips.front();
		FRHITextureCreateDesc Desc =
			FRHITextureCreateDesc::CreateCube("DTextureCube")
				.SetExtent(BaseMip.Width, BaseMip.Height)
				.SetFormat(PlatformData->PixelFormat)
				.SetNumMips(static_cast<uint8>(
					PlatformData->Faces[0].Mips.size()))
				.SetFlags(ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::CPUReadback);
		if (!GDynamicRHI->RHIIsTextureFormatSupported(Desc))
		{
			Completion->MarkFailed(
				Revision, ETextureRenderFailure::UnsupportedFormat);
			return;
		}

		auto& CommandList =
			static_cast<FRHICommandListImmediate&>(RHICmdList);
		FTextureRHIRef NewTexture =
			GDynamicRHI->RHICreateTexture(CommandList, Desc);
		if (NewTexture == nullptr)
		{
			Completion->MarkFailed(
				Revision, ETextureRenderFailure::CreateOrUpload);
			return;
		}

		for (uint32 FaceIndex = 0;
			FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			for (uint32 MipIndex = 0;
				MipIndex < PlatformData->Faces[FaceIndex].Mips.size();
				++MipIndex)
			{
				const FTexture2DMipData& Mip =
					PlatformData->Faces[FaceIndex].Mips[MipIndex];
				const FUpdateTextureRegion2D Region(
					0, 0, 0, 0, Mip.Width, Mip.Height);
				GDynamicRHI->RHIUpdateTexture2D(
					CommandList, NewTexture, MipIndex, FaceIndex,
					Region, Mip.RowPitch, Mip.Pixels.data());
			}
		}

		if (Revision != Completion->GetRequestedRevision()) return;
		SetTextureRHI_RenderThread(std::move(NewTexture));
		PublishTexture_RenderThread();
		Completion->MarkReady(Revision);
	}

	auto FTextureCubeResource::ReleaseRHI() -> void
	{
		check(IsInRenderingThread());
		FTextureResource::ReleaseRHI();
		Completion->MarkReleased(
			ReleaseRevision != 0 ? ReleaseRevision : Revision);
	}
}
