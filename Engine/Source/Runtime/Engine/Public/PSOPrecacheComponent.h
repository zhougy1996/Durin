#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Materials/MaterialPSOPrecache.h"

namespace Durin
{
	// GameThread policy for automatic component requests only. Explicit material
	// requests and draw-time resource preparation remain independent.
	// Automatic is off for editor hosts (including PIE), on for game hosts.
	enum class EComponentPSOPrecacheMode : uint8 { Automatic, Enabled, Disabled };
	ENGINE_API auto GetComponentPSOPrecacheMode() -> EComponentPSOPrecacheMode;
	// Wakes existing automatic owners; enabling takes effect on normal requests
	// rather than withdrawing already published proxies merely for warming.
	ENGINE_API auto SetComponentPSOPrecacheMode(EComponentPSOPrecacheMode Mode) -> void;
	ENGINE_API auto IsComponentPSOPrecachingEnabled() -> bool;

	class DMaterialInterface;
	struct FComponentPSOPrecacheParams
	{
		// Borrowed only during synchronous GameThread collection/submission.
		DMaterialInterface* MaterialInterface = nullptr;
		std::vector<FPSOPrecacheVertexFactoryData> VertexFactoryDataList;
		FPSOPrecacheParams PSOPrecacheParams;
		EPSOPrecachePriority Priority = EPSOPrecachePriority::Medium;
	};
	using FComponentPSOPrecacheParamsList = std::vector<FComponentPSOPrecacheParams>;
	// GameThread observer set. Completion is polled by its owner, never by a worker
	// holding a component pointer. Reset releases observers without canceling peers.
	class FPSOPrecacheComponentData
	{
	public:
		FPSOPrecacheComponentData() = default;
		ENGINE_API ~FPSOPrecacheComponentData();
		FPSOPrecacheComponentData(const FPSOPrecacheComponentData&) = delete;
		auto operator=(const FPSOPrecacheComponentData&) -> FPSOPrecacheComponentData& = delete;
		ENGINE_API auto SetMaterialPSOPrecacheData(std::vector<FMaterialPSOPrecacheRequestID> Requests,
			std::function<void()> Finished = {}, std::function<void()> Changed = {}) -> bool;
		ENGINE_API static auto DispatchOwnerNotifications() -> void;
		ENGINE_API auto Poll() -> EMaterialPSOPrecacheState;
		ENGINE_API auto Reset() -> void;
		ENGINE_API auto IsPSOPrecaching() const -> bool;
		ENGINE_API auto CheckPSOPrecachingAndBoostPriority(EPSOPrecachePriority Priority) -> bool;
		ENGINE_API auto IsReadyForProxyCreation() const -> bool;
		ENGINE_API auto SetLatestJobSetCompleted(uint64 JobSet) -> bool;
		auto IsPSOPrecacheCalled() const -> bool { return Called; }
		auto GetLatestJobSet() const -> uint64 { return LatestPSOPrecacheJobSet; }
		auto GetLatestJobSetCompleted() const -> uint64 { return LatestPSOPrecacheJobSetCompleted; }
		auto GetMaterialPSOPrecacheRequestIDs() const -> std::span<const FMaterialPSOPrecacheRequestID> { return MaterialPSOPrecacheRequestIDs; }
		static constexpr size_t MaximumRequests = 256;
	private:
		auto GetState() const -> EMaterialPSOPrecacheState;
		std::vector<FMaterialPSOPrecacheRequestID> MaterialPSOPrecacheRequestIDs;
		std::function<void()> Completion;
		std::vector<FMaterialPSOPrecacheSubscription> Subscriptions;
		std::shared_ptr<std::atomic<uint64>> CompletionEpoch;
		uint64 LatestPSOPrecacheJobSet = 0, LatestPSOPrecacheJobSetCompleted = 0;
		bool Called = false;
	};
}
