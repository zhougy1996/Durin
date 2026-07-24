#include "Texture/TextureCubeRenderResource.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	FTextureCubeRenderResource::~FTextureCubeRenderResource()
	{
		checkf(RequestedRevision.load(std::memory_order_relaxed) == 0 || IsInRenderingThread(),
			"Cube texture render resource left the rendering thread before destruction");
	}

	auto FTextureCubeRenderResource::QueueBuild(std::shared_ptr<const FTextureCubePlatformData> PlatformDataSnapshot, uint64 Revision) -> void
	{
		check(PlatformDataSnapshot && PlatformDataSnapshot->IsValid());
		RequestedRevision.store(Revision, std::memory_order_release);
		FailedRevision.store(0, std::memory_order_release);
		SetResourceState(ERenderResourceState::Pending, Revision);
		auto Self = shared_from_this();
		ENQUEUE_RENDER_COMMAND(BuildTextureCubeResource)([Self = std::move(Self), PlatformDataSnapshot = std::move(PlatformDataSnapshot), Revision](FRHICommandListImmediate& CommandList) {
			Self->Build_RenderThread(CommandList, *PlatformDataSnapshot, Revision);
		});
	}

	auto FTextureCubeRenderResource::QueueRelease(uint64 Revision) -> void
	{
		RequestedRevision.store(Revision, std::memory_order_release);
		FailedRevision.store(0, std::memory_order_release);
		SetResourceState(ERenderResourceState::Pending, Revision);
		auto Self = shared_from_this();
		ENQUEUE_RENDER_COMMAND(ReleaseTextureCubeResource)([Self = std::move(Self), Revision](FRHICommandListImmediate&) {
			Self->Release_RenderThread(Revision);
		});
	}

	auto FTextureCubeRenderResource::GetTextureRHI_RenderThread() const -> FRHITexture*
	{
		check(IsInRenderingThread());
		return IsReady_RenderThread() ? TextureRHI.GetReference() : nullptr;
	}

	auto FTextureCubeRenderResource::IsReady_RenderThread() const -> bool
	{
		check(IsInRenderingThread());
		return TextureRHI != nullptr && AppliedRevision == RequestedRevision.load(std::memory_order_acquire);
	}

	auto FTextureCubeRenderResource::GetAppliedRevision_RenderThread() const -> uint64
	{
		check(IsInRenderingThread());
		return AppliedRevision;
	}

	auto FTextureCubeRenderResource::GetResourceState() const -> ERenderResourceState
	{
		std::lock_guard Lock(ResourceStateMutex);
		const uint64 Requested = RequestedRevision.load(std::memory_order_acquire);
		if (ResourceStateRevision != Requested) return ERenderResourceState::Pending;
		return ResourceState;
	}

	auto FTextureCubeRenderResource::SetResourceState(ERenderResourceState State, uint64 Revision) -> void
	{
		std::lock_guard Lock(ResourceStateMutex);
		ResourceState = State;
		ResourceStateRevision = Revision;
	}

	auto FTextureCubeRenderResource::Build_RenderThread(FRHICommandListImmediate& CommandList,
		const FTextureCubePlatformData& PlatformData, uint64 Revision) -> void
	{
		check(IsInRenderingThread());
		if (Revision != RequestedRevision.load(std::memory_order_acquire)) return;
		SetResourceState(ERenderResourceState::Building, Revision);

		const FTexture2DMipData& BaseMip = PlatformData.Faces[0].Mips.front();
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::CreateCube("DTextureCube")
			.SetExtent(BaseMip.Width, BaseMip.Height)
			.SetFormat(PlatformData.PixelFormat)
			.SetNumMips(static_cast<uint8>(PlatformData.Faces[0].Mips.size()))
			.SetFlags(ETextureCreateFlags::ShaderResource);
		FTextureRHIRef NewTexture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
		if (NewTexture == nullptr)
		{
			FailedRevision.store(Revision, std::memory_order_release);
			SetResourceState(ERenderResourceState::Failed, Revision);
			return;
		}

		for (uint32 FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
		{
			for (uint32 MipIndex = 0; MipIndex < PlatformData.Faces[FaceIndex].Mips.size(); ++MipIndex)
			{
				const FTexture2DMipData& Mip = PlatformData.Faces[FaceIndex].Mips[MipIndex];
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, Mip.Width, Mip.Height);
				GDynamicRHI->RHIUpdateTexture2D(CommandList, NewTexture, MipIndex, FaceIndex, Region, Mip.RowPitch, Mip.Pixels.data());
			}
		}

		if (Revision != RequestedRevision.load(std::memory_order_acquire)) return;
		TextureRHI = std::move(NewTexture);
		AppliedRevision = Revision;
		FailedRevision.store(0, std::memory_order_release);
		SetResourceState(ERenderResourceState::Ready, Revision);
	}

	auto FTextureCubeRenderResource::Release_RenderThread(uint64 Revision) -> void
	{
		check(IsInRenderingThread());
		if (Revision != RequestedRevision.load(std::memory_order_acquire)) return;
		TextureRHI = nullptr;
		AppliedRevision = Revision;
		FailedRevision.store(0, std::memory_order_release);
		SetResourceState(ERenderResourceState::Released, Revision);
	}
}
