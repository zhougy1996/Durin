#pragma once

#include "EngineAPI.h"
#include "RenderResource.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	struct FTextureCubePlatformData;

	// Carries revision-tagged render completion back to the producer without
	// sharing ownership of the concrete resource.
	class FTextureCubeResourceCompletion final
	{
	public:
		ENGINE_API auto BeginRequest(uint64 Revision) -> void;
		ENGINE_API auto MarkBuilding(uint64 Revision) -> bool;
		ENGINE_API auto MarkReady(uint64 Revision) -> void;
		ENGINE_API auto MarkFailed(
			uint64 Revision, ETextureRenderFailure Reason) -> void;
		ENGINE_API auto MarkReleased(uint64 Revision) -> void;

		ENGINE_API auto GetResourceState() const -> ERenderResourceState;
		auto GetRequestedRevision() const -> uint64
		{
			return RequestedRevision.load(std::memory_order_acquire);
		}
		auto GetAppliedRevision() const -> uint64
		{
			return AppliedRevision.load(std::memory_order_acquire);
		}
		auto GetFailedRevision() const -> uint64
		{
			return FailedRevision.load(std::memory_order_acquire);
		}
		auto GetFailureReason() const -> ETextureRenderFailure
		{
			return FailureReason.load(std::memory_order_acquire);
		}

	private:
		auto SetResourceState(
			ERenderResourceState State, uint64 Revision) -> void;

		std::atomic<uint64> RequestedRevision = 0;
		std::atomic<uint64> AppliedRevision = 0;
		std::atomic<uint64> FailedRevision = 0;
		std::atomic<ETextureRenderFailure> FailureReason =
			ETextureRenderFailure::None;
		mutable std::mutex ResourceStateMutex;
		ERenderResourceState ResourceState = ERenderResourceState::Idle;
		uint64 ResourceStateRevision = 0;
	};

	// One asset-owned cube allocation. Only render commands access its RHI state.
	class FTextureCubeResource final : public FTextureResource
	{
	public:
		ENGINE_API FTextureCubeResource(
			FTextureReference* InTextureReference,
			std::shared_ptr<const FTextureCubePlatformData> InPlatformData,
			uint64 InRevision,
			std::shared_ptr<FTextureCubeResourceCompletion> InCompletion);
		ENGINE_API ~FTextureCubeResource() override;

		auto PrepareForRelease(uint64 InReleaseRevision) -> void
		{
			ReleaseRevision = InReleaseRevision;
		}
		ENGINE_API auto InitRHI(FRHICommandListBase& RHICmdList) -> void override;
		ENGINE_API auto ReleaseRHI() -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FTextureCubeResource";
		}

	private:
		std::shared_ptr<const FTextureCubePlatformData> PlatformData;
		uint64 Revision = 0;
		uint64 ReleaseRevision = 0;
		std::shared_ptr<FTextureCubeResourceCompletion> Completion;
	};
}
