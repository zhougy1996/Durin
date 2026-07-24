#include "Texture/Texture2DRenderResource.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	FTexture2DRenderResource::~FTexture2DRenderResource()
	{
		// Once used by the RHI, the final shared owner must always be a render command.
		checkf(RequestedRevision.load(std::memory_order_relaxed) == 0 || IsInRenderingThread(),
			"Texture render resource left the rendering thread before destruction");
	}

	auto FTexture2DRenderResource::QueueBuild(std::shared_ptr<const FTexturePlatformData> PlatformDataSnapshot, uint64 Revision) -> void
	{
		check(PlatformDataSnapshot && PlatformDataSnapshot->IsValid());
		RequestedRevision.store(Revision, std::memory_order_release);
		FailedRevision.store(0, std::memory_order_release);
		SetResourceState(ERenderResourceState::Pending, Revision);
		auto Self = shared_from_this();
		ENQUEUE_RENDER_COMMAND(BuildTexture2DResource)([Self = std::move(Self), PlatformDataSnapshot = std::move(PlatformDataSnapshot), Revision](FRHICommandListImmediate& CommandList) {
			Self->Build_RenderThread(CommandList, *PlatformDataSnapshot, Revision);
		});
	}

	auto FTexture2DRenderResource::QueueRelease(uint64 Revision) -> void
	{
		RequestedRevision.store(Revision, std::memory_order_release);
		FailedRevision.store(0, std::memory_order_release);
		SetResourceState(ERenderResourceState::Pending, Revision);
		auto Self = shared_from_this();
		ENQUEUE_RENDER_COMMAND(ReleaseTexture2DResource)([Self = std::move(Self), Revision](FRHICommandListImmediate&) {
			Self->Release_RenderThread(Revision);
		});
	}

	auto FTexture2DRenderResource::GetTextureRHI_RenderThread() const -> FRHITexture*
	{
		check(IsInRenderingThread());
		return IsReady_RenderThread() ? TextureRHI.GetReference() : nullptr;
	}

	auto FTexture2DRenderResource::IsReady_RenderThread() const -> bool
	{
		check(IsInRenderingThread());
		return TextureRHI != nullptr && AppliedRevision == RequestedRevision.load(std::memory_order_acquire);
	}

	auto FTexture2DRenderResource::GetAppliedRevision_RenderThread() const -> uint64
	{
		check(IsInRenderingThread());
		return AppliedRevision;
	}

	auto FTexture2DRenderResource::GetResourceState() const -> ERenderResourceState
	{
		std::lock_guard Lock(ResourceStateMutex);
		const uint64 Requested = RequestedRevision.load(std::memory_order_acquire);
		if (ResourceStateRevision != Requested) return ERenderResourceState::Pending;
		return ResourceState;
	}

	auto FTexture2DRenderResource::SetResourceState(ERenderResourceState State, uint64 Revision) -> void
	{
		std::lock_guard Lock(ResourceStateMutex);
		ResourceState = State;
		ResourceStateRevision = Revision;
	}

	auto FTexture2DRenderResource::Build_RenderThread(FRHICommandListImmediate& CommandList, const FTexturePlatformData& PlatformData, uint64 Revision) -> void
	{
		check(IsInRenderingThread());
		if (Revision != RequestedRevision.load(std::memory_order_acquire)) return;

		SetResourceState(ERenderResourceState::Building, Revision);
		const FTexture2DMipData& BaseMip = PlatformData.Mips.front();
		FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D("DTexture2D", BaseMip.Width, BaseMip.Height, PlatformData.PixelFormat)
			.SetNumMips(static_cast<uint8>(PlatformData.Mips.size()))
			.SetFlags(ETextureCreateFlags::ShaderResource);
		FTextureRHIRef NewTexture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
		if (NewTexture == nullptr)
		{
			FailedRevision.store(Revision, std::memory_order_release);
			SetResourceState(ERenderResourceState::Failed, Revision);
			return;
		}

		for (uint32 MipIndex = 0; MipIndex < PlatformData.Mips.size(); ++MipIndex)
		{
			const FTexture2DMipData& Mip = PlatformData.Mips[MipIndex];
			const FUpdateTextureRegion2D Region(0, 0, 0, 0, Mip.Width, Mip.Height);
			GDynamicRHI->RHIUpdateTexture2D(CommandList, NewTexture, MipIndex, Region, Mip.RowPitch, Mip.Pixels.data());
		}

		// A newer request may have arrived while this command was executing.
		if (Revision != RequestedRevision.load(std::memory_order_acquire)) return;
		TextureRHI = std::move(NewTexture);
		AppliedRevision = Revision;
		FailedRevision.store(0, std::memory_order_release);
		SetResourceState(ERenderResourceState::Ready, Revision);
	}

	auto FTexture2DRenderResource::Release_RenderThread(uint64 Revision) -> void
	{
		check(IsInRenderingThread());
		if (Revision != RequestedRevision.load(std::memory_order_acquire)) return;
		TextureRHI = nullptr;
		AppliedRevision = Revision;
		FailedRevision.store(0, std::memory_order_release);
		SetResourceState(ERenderResourceState::Released, Revision);
	}
}
