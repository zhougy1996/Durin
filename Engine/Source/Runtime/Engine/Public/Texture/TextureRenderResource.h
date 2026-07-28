#pragma once

#include "EngineAPI.h"
#include "RenderResource.h"
#include "Texture/Texture.h"

namespace Durin
{
	// Carries revision-tagged render completion back to a texture asset without
	// sharing ownership of the concrete resource.
	class FTextureResourceCompletion final
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
		auto AdvanceAppliedRevision(uint64 Revision) -> void;

		std::atomic<uint64> RequestedRevision = 0;
		std::atomic<uint64> AppliedRevision = 0;
		std::atomic<uint64> FailedRevision = 0;
		std::atomic<ETextureRenderFailure> FailureReason =
			ETextureRenderFailure::None;
		mutable std::mutex ResourceStateMutex;
		ERenderResourceState ResourceState = ERenderResourceState::Idle;
		uint64 ResourceStateRevision = 0;
	};

	// Common revision and completion contract for an asset-owned texture
	// allocation. Concrete resources retain only topology-specific snapshots and
	// upload logic.
	class FTextureAssetResource : public FTextureResource
	{
	public:
		ENGINE_API FTextureAssetResource(
			FTextureReference* InTextureReference,
			uint64 InRevision,
			std::shared_ptr<FTextureResourceCompletion> InCompletion);
		ENGINE_API ~FTextureAssetResource() override;

		// Used only when this resource is the current allocation being invalidated.
		auto PrepareForRelease(uint64 InReleaseRevision) -> void
		{
			ReleaseRevision = InReleaseRevision;
		}
		auto InitRHI(FRHICommandListBase& RHICmdList) -> void override = 0;
		ENGINE_API auto ReleaseRHI() -> void override;

	protected:
		auto GetRevision() const -> uint64
		{
			return Revision;
		}
		auto GetCompletion() const -> const std::shared_ptr<FTextureResourceCompletion>&
		{
			return Completion;
		}

	private:
		uint64 Revision = 0;
		uint64 ReleaseRevision = 0;
		std::shared_ptr<FTextureResourceCompletion> Completion;
	};
}
