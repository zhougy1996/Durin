#pragma once

#include "EngineAPI.h"
#include "RHIResources.h"

namespace Durin
{
	struct FTexturePlatformData;

	// Render-thread state for the texture's GPU resource.
	enum class ERenderResourceState : uint8
	{
		Idle,      // No build/release requested yet
		Pending,   // Queued build/release not yet consumed by render thread
		Building,  // Render thread is actively building
		Ready,     // Fully built and revision matches
		Failed,    // RHI texture creation or upload failed
		Released,  // Explicitly released
	};

	// Cross-thread lifetime proxy for DTexture2D. Only render commands may touch TextureRHI.
	class ENGINE_API FTexture2DRenderResource final : public std::enable_shared_from_this<FTexture2DRenderResource>
	{
	public:
		FTexture2DRenderResource() = default;
		~FTexture2DRenderResource();

		FTexture2DRenderResource(const FTexture2DRenderResource&) = delete;
		auto operator=(const FTexture2DRenderResource&) -> FTexture2DRenderResource& = delete;

		auto QueueBuild(std::shared_ptr<const FTexturePlatformData> PlatformDataSnapshot, uint64 Revision) -> void;
		auto QueueRelease(uint64 Revision) -> void;

		// These accessors are render-thread-only. Callers must resolve nullptr to a renderer default texture.
		auto GetTextureRHI_RenderThread() const -> FRHITexture*;
		auto IsReady_RenderThread() const -> bool;
		auto GetAppliedRevision_RenderThread() const -> uint64;

		// Thread-safe state query for game-thread diagnostics.
		auto GetResourceState() const -> ERenderResourceState { return ResourceState.load(std::memory_order_acquire); }

		// Write-once failure flag set by the render thread; safe for game-thread poll.
		std::atomic<bool> bFailed{false};

	private:
		auto Build_RenderThread(FRHICommandListImmediate& CommandList, const FTexturePlatformData& PlatformData, uint64 Revision) -> void;
		auto Release_RenderThread(uint64 Revision) -> void;

		FTextureRHIRef TextureRHI;
		uint64 AppliedRevision = 0;
		std::atomic<uint64> RequestedRevision = 0;
		std::atomic<ERenderResourceState> ResourceState{ERenderResourceState::Idle};
	};
}
