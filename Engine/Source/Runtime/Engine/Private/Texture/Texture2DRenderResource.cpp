#include "Texture/Texture2DRenderResource.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	auto FTexture2DResourceCompletion::BeginRequest(uint64 Revision) -> void
	{
		RequestedRevision.store(Revision, std::memory_order_release);
		FailedRevision.store(0, std::memory_order_release);
		FailureReason.store(
			ETextureRenderFailure::None, std::memory_order_release);
		SetResourceState(ERenderResourceState::Pending, Revision);
	}

	auto FTexture2DResourceCompletion::MarkBuilding(uint64 Revision) -> bool
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return false;
		SetResourceState(ERenderResourceState::Building, Revision);
		return true;
	}

	auto FTexture2DResourceCompletion::MarkReady(uint64 Revision) -> void
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return;
		AppliedRevision.store(Revision, std::memory_order_release);
		FailedRevision.store(0, std::memory_order_release);
		FailureReason.store(
			ETextureRenderFailure::None, std::memory_order_release);
		SetResourceState(ERenderResourceState::Ready, Revision);
	}

	auto FTexture2DResourceCompletion::MarkFailed(
		uint64 Revision, ETextureRenderFailure Reason) -> void
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return;
		FailureReason.store(Reason, std::memory_order_release);
		FailedRevision.store(Revision, std::memory_order_release);
		SetResourceState(ERenderResourceState::Failed, Revision);
	}

	auto FTexture2DResourceCompletion::MarkReleased(uint64 Revision) -> void
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return;
		AppliedRevision.store(Revision, std::memory_order_release);
		FailedRevision.store(0, std::memory_order_release);
		FailureReason.store(
			ETextureRenderFailure::None, std::memory_order_release);
		SetResourceState(ERenderResourceState::Released, Revision);
	}

	auto FTexture2DResourceCompletion::GetResourceState() const
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

	auto FTexture2DResourceCompletion::SetResourceState(
		ERenderResourceState State, uint64 Revision) -> void
	{
		std::lock_guard Lock(ResourceStateMutex);
		ResourceState = State;
		ResourceStateRevision = Revision;
	}

	FTexture2DResource::FTexture2DResource(
		FTextureReference* InTextureReference,
		std::shared_ptr<const FTexturePlatformData> InPlatformData,
		uint64 InRevision,
		std::shared_ptr<FTexture2DResourceCompletion> InCompletion)
		: FTextureResource(InTextureReference)
		, PlatformData(std::move(InPlatformData))
		, Revision(InRevision)
		, Completion(std::move(InCompletion))
	{
		check(PlatformData && PlatformData->IsValid());
		check(Completion != nullptr);
	}

	FTexture2DResource::~FTexture2DResource() = default;

	auto FTexture2DResource::InitRHI(FRHICommandListBase& RHICmdList) -> void
	{
		check(IsInRenderingThread());
		if (!Completion->MarkBuilding(Revision)) return;

		const FTexture2DMipData& BaseMip = PlatformData->Mips.front();
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
			"DTexture2D", BaseMip.Width, BaseMip.Height,
			PlatformData->PixelFormat)
			.SetNumMips(static_cast<uint8>(PlatformData->Mips.size()))
			.SetFlags(ETextureCreateFlags::ShaderResource);
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

		for (uint32 MipIndex = 0;
			MipIndex < PlatformData->Mips.size(); ++MipIndex)
		{
			const FTexture2DMipData& Mip = PlatformData->Mips[MipIndex];
			const FUpdateTextureRegion2D Region(
				0, 0, 0, 0, Mip.Width, Mip.Height);
			GDynamicRHI->RHIUpdateTexture2D(
				CommandList, NewTexture, MipIndex, 0, Region,
				Mip.RowPitch, Mip.Pixels.data());
		}

		if (Revision != Completion->GetRequestedRevision()) return;
		SetTextureRHI_RenderThread(std::move(NewTexture));
		PublishTexture_RenderThread();
		Completion->MarkReady(Revision);
	}

	auto FTexture2DResource::ReleaseRHI() -> void
	{
		check(IsInRenderingThread());
		FTextureResource::ReleaseRHI();
		Completion->MarkReleased(
			ReleaseRevision != 0 ? ReleaseRevision : Revision);
	}
}
