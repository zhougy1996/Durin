#pragma once

#include "EngineAPI.h"
#include "DObject/ObjectKey.h"

namespace Durin
{
	class DWorld;

	// Identifies one timer incarnation in one World; clearing never revives old handles.
	struct FTimerHandle
	{
		FObjectKey World;
		uint32 Slot = 0;
		uint64 Generation = 0;
		friend auto operator==(const FTimerHandle&, const FTimerHandle&) -> bool = default;
	};

	// Game-thread-only, non-retaining object bindings and play-scoped gameplay timers.
	class FTimerManager
	{
	public:
		ENGINE_API ~FTimerManager();
		FTimerManager(const FTimerManager&) = delete;
		auto operator=(const FTimerManager&) -> FTimerManager& = delete;

		// Returns an invalid handle for invalid input or outside BeginningPlay/Playing.
		// Delay is finite and nonnegative; Interval == 0 is one-shot, otherwise positive.
		// New timers never execute in the gameplay frame that registered them.
		ENGINE_API auto SetTimer(double Delay, std::move_only_function<void()> Callback, double Interval = 0.0) -> FTimerHandle;
		ENGINE_API auto SetTimerForNextTick(std::move_only_function<void()> Callback) -> FTimerHandle;
		// Owner is resolved and lifecycle-checked immediately before invocation, without GC retention.
		ENGINE_API auto SetTimerForObject(DObject* Owner, double Delay,
			std::move_only_function<void(DObject&)> Callback, double Interval = 0.0) -> FTimerHandle;
		ENGINE_API auto SetTimerForObjectNextTick(DObject* Owner, std::move_only_function<void(DObject&)> Callback) -> FTimerHandle;
		ENGINE_API auto ClearTimer(FTimerHandle Handle) -> bool;
		ENGINE_API auto ClearAllTimersForObject(const DObject* Owner) -> void;
		ENGINE_API auto PauseTimer(FTimerHandle Handle) -> bool;
		// Resuming retains remaining time and waits until at least the next gameplay frame.
		ENGINE_API auto UnpauseTimer(FTimerHandle Handle) -> bool;
		ENGINE_API auto IsTimerActive(FTimerHandle Handle) const -> bool;
		ENGINE_API auto IsTimerPaused(FTimerHandle Handle) const -> bool;
		// Empty for stale/inactive handles; zero for next-frame timers and currently due timers.
		ENGINE_API auto GetTimerRemaining(FTimerHandle Handle) const -> std::optional<double>;
		ENGINE_API auto GetGameTimeSeconds() const -> double;

	private:
		// World is the sole constructor authority, so World identity also identifies the manager.
		ENGINE_API explicit FTimerManager(DWorld& InWorld);
		struct FState;
		std::unique_ptr<FState> State;
		auto Add(double Delay, double Interval, bool bNextFrame, DObject* Owner,
			std::move_only_function<void(DObject*)> Callback) -> FTimerHandle;
		auto StartFrame(double DeltaSeconds) -> void;
		auto Dispatch() -> void;
		auto Reset() -> void;
		friend class DWorld;
	};
}
