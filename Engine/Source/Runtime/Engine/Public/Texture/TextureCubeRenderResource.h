#pragma once

#include "EngineAPI.h"
#include "RHIResources.h"
#include "Texture/Texture2D.h"

	namespace Durin
{
	struct FTextureCubePlatformData;

	// Owns one revisioned cube RHI resource exclusively through render-thread commands.
	class FTextureCubeRenderResource final : public std::enable_shared_from_this<FTextureCubeRenderResource>
	{
	public:
		ENGINE_API static auto Create() -> std::shared_ptr<FTextureCubeRenderResource>;
		ENGINE_API ~FTextureCubeRenderResource();

		FTextureCubeRenderResource(const FTextureCubeRenderResource&) = delete;
		auto operator=(const FTextureCubeRenderResource&) -> FTextureCubeRenderResource& = delete;

		ENGINE_API auto QueueBuild(std::shared_ptr<const FTextureCubePlatformData> PlatformDataSnapshot, uint64 Revision) -> void;
		ENGINE_API auto QueueRelease(uint64 Revision) -> void;

		ENGINE_API auto GetTextureRHI_RenderThread() const -> FRHITexture*;
		ENGINE_API auto IsReady_RenderThread() const -> bool;
		ENGINE_API auto GetAppliedRevision_RenderThread() const -> uint64;
		ENGINE_API auto GetResourceState() const -> ERenderResourceState;
		auto GetRequestedRevision() const -> uint64
		{
			return RequestedRevision.load(std::memory_order_acquire);
		}
		auto GetFailedRevision() const -> uint64 { return FailedRevision.load(std::memory_order_acquire); }
		auto GetFailureReason() const -> ETextureRenderFailure { return FailureReason.load(std::memory_order_acquire); }

	private:
		FTextureCubeRenderResource() = default;

		auto Build_RenderThread(FRHICommandListImmediate& CommandList, const FTextureCubePlatformData& PlatformData, uint64 Revision) -> void;
		auto Release_RenderThread(uint64 Revision) -> void;
		auto SetResourceState(ERenderResourceState State, uint64 Revision) -> void;

		FTextureRHIRef TextureRHI;
		uint64 AppliedRevision = 0;
		std::atomic<uint64> RequestedRevision = 0;
		std::atomic<uint64> FailedRevision = 0;
		std::atomic<ETextureRenderFailure> FailureReason = ETextureRenderFailure::None;
		mutable std::mutex ResourceStateMutex;
		ERenderResourceState ResourceState = ERenderResourceState::Idle;
		uint64 ResourceStateRevision = 0;
	};
}
