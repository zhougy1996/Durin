#pragma once

#include <functional>
#include <unordered_map>
#include <vector>

#include "DObject/WeakObjectPtr.h"
#include "EngineAPI.h"
#include "Rendering/PrimitiveComponentId.h"
#include "PrimitiveSceneChanges.gen.h"

namespace Durin
{
#if DURIN_WITH_EDITOR
	class AActor;
	class DLevel;
	class DPrimitiveComponent;

	// Game-thread invalidation facts; consumers capture their own derived data synchronously.
	DSTRUCT()
	struct FPrimitiveSceneMutation
	{
		GENERATED_BODY()
		DPROPERTY(Transient)
		TWeakObjectPtr<AActor> Actor;
		DPROPERTY(Transient)
		TWeakObjectPtr<DPrimitiveComponent> Component;
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		uint64 RegistrationGeneration = 0;
		bool bRetired = false;
	};

	DSTRUCT()
	struct FPrimitiveSceneMutationBatch
	{
		GENERATED_BODY()
		uint64 Revision = 0;
		bool bCompleteSnapshot = false;
		DPROPERTY(Transient)
		std::vector<FPrimitiveSceneMutation> Mutations;
	};

	// Optional editor-build observation, independent of render-scene presence or query policy.
	// Owned by one Level; observers must not mutate scene state during synchronous dispatch.
	class FPrimitiveSceneChanges final
	{
	public:
		using FObserver = std::function<void(const FPrimitiveSceneMutationBatch&)>;
		explicit FPrimitiveSceneChanges(DLevel& InLevel) : Level(InLevel) {}
		FPrimitiveSceneChanges(const FPrimitiveSceneChanges&) = delete;
		auto operator=(const FPrimitiveSceneChanges&) -> FPrimitiveSceneChanges& = delete;
		// Supplies a complete initial snapshot before returning the subscription.
		ENGINE_API auto Subscribe(FObserver Observer) -> uint64;
		ENGINE_API auto Unsubscribe(uint64 Subscription) -> void;
		ENGINE_API auto CaptureSnapshot() const -> FPrimitiveSceneMutationBatch;

	private:
		auto Notify(DPrimitiveComponent* Component, bool bRetired) -> void;
		DLevel& Level;
		uint64 Revision = 1;
		uint64 NextObserverId = 1;
		bool bDispatching = false;
		std::unordered_map<uint64, FObserver> Observers;
		friend class DPrimitiveComponent;
	};
#endif
}
