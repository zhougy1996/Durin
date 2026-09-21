#include "Engine/TimerManager.h"
#include "Engine/World.h"
#include "Engine/Actor.h"
#include "Components/ActorComponent.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include <deque>

namespace Durin
{
	namespace
	{
		auto RequireTimerThread() -> void { require(!GIsGameThreadIdInitialized || IsInGameThread()); }

		auto IsTimerOwnerEligible(DObject* Object, DWorld& World) -> bool
		{
			if (!Object) return false;
			for (DObject* Current = Object; Current; Current = Current->GetOuter())
			{
				if (Current->IsPendingKill()) return false;
				if (Current == &World) return true;
				if (auto* Component = Cast<DActorComponent>(Current))
				{
					if (!Component->IsRegistered() || !Component->HasBegunPlay()
						|| Component->IsEndingPlay() || Component->IsBeingDestroyed()
						|| !Component->GetOwner() || !Component->GetOwner()->OwnsComponent(Component)) return false;
				}
				if (auto* Actor = Cast<AActor>(Current))
					if (!World.ContainsActor(Actor) || !Actor->HasBegunPlay()
						|| Actor->IsEndingPlay() || Actor->IsBeingDestroyed()) return false;
				if (auto* Level = Cast<DLevel>(Current))
					return World.GetCurrentLevel() == Level && Level->GetWorld() == &World;
			}
			return false;
		}
	}

	struct FTimerManager::FState
	{
		struct FTimer
		{
			FTimerHandle Handle;
			FWeakObjectPtr Owner;
			std::move_only_function<void(DObject*)> Callback;
			double Deadline = 0.0;
			double Interval = 0.0;
			double Remaining = 0.0;
			uint64 Revision = 0;
			bool bActive = true;
			bool bPaused = false;
			bool bNextFrame = false;
		};
		// Queue records are immutable; revisions invalidate pause/resume and cancelled entries.
		struct FQueued
		{
			std::shared_ptr<FTimer> Timer;
			uint64 Revision;
			double Deadline;
			auto IsCurrent() const -> bool
			{
				return Timer->bActive && !Timer->bPaused && Timer->Revision == Revision;
			}
		};
		static auto Later(const FQueued& A, const FQueued& B) -> bool
		{
			if (A.Deadline != B.Deadline) return A.Deadline > B.Deadline;
			return A.Timer->Handle.Generation > B.Timer->Handle.Generation;
		}
		DWorld& World;
		double Time = 0.0;
		uint64 Generation = 0;
		std::vector<std::shared_ptr<FTimer>> Slots;
		std::vector<uint32> FreeSlots;
		std::vector<FQueued> Pending;
		std::vector<FQueued> Heap;
		std::deque<FQueued> Ready;

		explicit FState(DWorld& InWorld) : World(InWorld) {}
		auto Find(FTimerHandle Handle) const -> std::shared_ptr<FTimer>
		{
			RequireTimerThread();
			if (Handle.Generation == 0 || Handle.World != FObjectKey(&World) || Handle.Slot >= Slots.size()) return {};
			auto Timer = Slots[Handle.Slot];
			return Timer && Timer->Handle == Handle && Timer->bActive ? Timer : nullptr;
		}
		auto Queue(const std::shared_ptr<FTimer>& Timer) -> void
		{
			Pending.push_back({Timer, Timer->Revision, Timer->Deadline});
			if (Pending.size() > Slots.size() * 2 + 64)
				std::erase_if(Pending, [](const auto& Entry) { return !Entry.IsCurrent(); });
		}
		auto Remove(const std::shared_ptr<FTimer>& Timer) -> void
		{
			if (!Timer->bActive) return;
			Timer->bActive = false;
			Slots[Timer->Handle.Slot].reset();
			FreeSlots.push_back(Timer->Handle.Slot);
			// Retire state before releasing user captures, which can themselves own cleanup actions.
			Timer->Callback = {};
		}
	};

	FTimerManager::FTimerManager(DWorld& InWorld) : State(std::make_unique<FState>(InWorld)) {}
	FTimerManager::~FTimerManager() = default;

	auto FTimerManager::Add(double Delay, double Interval, bool bNextFrame, DObject* Owner,
		std::move_only_function<void(DObject*)> Callback) -> FTimerHandle
	{
		RequireTimerThread();
		auto& S = *State;
		if (!Callback || !std::isfinite(Delay) || Delay < 0.0 || !std::isfinite(Interval) || Interval < 0.0
			|| !std::isfinite(S.Time + Delay) || !S.World.CanDispatchSubsystems()
			|| (S.World.PlayState != EWorldPlayState::BeginningPlay && S.World.PlayState != EWorldPlayState::Playing)
			|| (Owner && !IsTimerOwnerEligible(Owner, S.World))) return {};
		auto Timer = std::make_shared<FState::FTimer>();
		uint32 Slot;
		if (S.FreeSlots.empty()) { Slot = static_cast<uint32>(S.Slots.size()); S.Slots.push_back({}); }
		else { Slot = S.FreeSlots.back(); S.FreeSlots.pop_back(); }
		Timer->Handle = {FObjectKey(&S.World), Slot, ++S.Generation};
		Timer->Owner = FWeakObjectPtr(Owner);
		Timer->Callback = std::move(Callback);
		Timer->Deadline = S.Time + Delay;
		Timer->Interval = Interval;
		Timer->bNextFrame = bNextFrame;
		S.Slots[Slot] = Timer;
		S.Queue(Timer);
		return Timer->Handle;
	}

	auto FTimerManager::SetTimer(double Delay, std::move_only_function<void()> Callback, double Interval) -> FTimerHandle
	{
		RequireTimerThread();
		if (!Callback) return {};
		return Add(Delay, Interval, false, nullptr, [Callback = std::move(Callback)](DObject*) mutable { Callback(); });
	}
	auto FTimerManager::SetTimerForNextTick(std::move_only_function<void()> Callback) -> FTimerHandle
	{
		RequireTimerThread();
		if (!Callback) return {};
		return Add(0.0, 0.0, true, nullptr, [Callback = std::move(Callback)](DObject*) mutable { Callback(); });
	}
	auto FTimerManager::SetTimerForObject(DObject* Owner, double Delay, std::move_only_function<void(DObject&)> Callback, double Interval) -> FTimerHandle
	{
		RequireTimerThread();
		if (!Owner || !Callback) return {};
		return Add(Delay, Interval, false, Owner, [Callback = std::move(Callback)](DObject* Target) mutable { Callback(*Target); });
	}
	auto FTimerManager::SetTimerForObjectNextTick(DObject* Owner, std::move_only_function<void(DObject&)> Callback) -> FTimerHandle
	{
		RequireTimerThread();
		if (!Owner || !Callback) return {};
		return Add(0.0, 0.0, true, Owner, [Callback = std::move(Callback)](DObject* Target) mutable { Callback(*Target); });
	}
	auto FTimerManager::ClearTimer(FTimerHandle Handle) -> bool
	{
		auto Timer = State->Find(Handle);
		if (!Timer) return false;
		State->Remove(Timer);
		return true;
	}
	auto FTimerManager::ClearAllTimersForObject(const DObject* Owner) -> void
	{
		RequireTimerThread();
		if (!Owner) return;
		const auto Identity = FObjectKey(const_cast<DObject*>(Owner));
		std::vector<FTimerHandle> Handles;
		for (const auto& Timer : State->Slots)
			if (Timer && Timer->Owner.GetKey() == Identity) Handles.push_back(Timer->Handle);
		for (const auto Handle : Handles) ClearTimer(Handle);
	}
	auto FTimerManager::PauseTimer(FTimerHandle Handle) -> bool
	{
		auto Timer = State->Find(Handle);
		if (!Timer || Timer->bPaused) return false;
		Timer->Remaining = std::max(0.0, Timer->Deadline - State->Time);
		Timer->bPaused = true;
		++Timer->Revision;
		return true;
	}
	auto FTimerManager::UnpauseTimer(FTimerHandle Handle) -> bool
	{
		auto Timer = State->Find(Handle);
		if (!Timer || !Timer->bPaused || !std::isfinite(State->Time + Timer->Remaining)) return false;
		Timer->Deadline = State->Time + Timer->Remaining;
		Timer->bPaused = false;
		++Timer->Revision;
		State->Queue(Timer);
		return true;
	}
	auto FTimerManager::IsTimerActive(FTimerHandle Handle) const -> bool { return State->Find(Handle) != nullptr; }
	auto FTimerManager::IsTimerPaused(FTimerHandle Handle) const -> bool
	{
		auto Timer = State->Find(Handle);
		return Timer && Timer->bPaused;
	}
	auto FTimerManager::GetTimerRemaining(FTimerHandle Handle) const -> std::optional<double>
	{
		auto Timer = State->Find(Handle);
		if (!Timer) return {};
		return Timer->bNextFrame ? 0.0 : Timer->bPaused ? Timer->Remaining : std::max(0.0, Timer->Deadline - State->Time);
	}
	auto FTimerManager::GetGameTimeSeconds() const -> double { RequireTimerThread(); return State->Time; }

	auto FTimerManager::StartFrame(double DeltaSeconds) -> void
	{
		RequireTimerThread();
		auto& S = *State;
		S.Time += DeltaSeconds;
		// Pending admission is frozen before input and Tick callbacks can create more work.
		for (auto& Entry : S.Pending)
		{
			if (!Entry.IsCurrent()) continue;
			if (Entry.Timer->bNextFrame) S.Ready.push_back(std::move(Entry));
			else { S.Heap.push_back(std::move(Entry)); std::push_heap(S.Heap.begin(), S.Heap.end(), FState::Later); }
		}
		S.Pending.clear();
		if (S.Heap.size() > S.Slots.size() * 2 + 64)
		{
			std::erase_if(S.Heap, [](const auto& Entry) { return !Entry.IsCurrent(); });
			std::make_heap(S.Heap.begin(), S.Heap.end(), FState::Later);
		}
	}

	auto FTimerManager::Dispatch() -> void
	{
		RequireTimerThread();
		auto& S = *State;
		while (!S.Heap.empty() && S.Heap.front().Deadline <= S.Time)
		{
			std::pop_heap(S.Heap.begin(), S.Heap.end(), FState::Later);
			auto Entry = std::move(S.Heap.back()); S.Heap.pop_back();
			if (Entry.IsCurrent()) S.Ready.push_back(std::move(Entry));
		}
		std::sort(S.Ready.begin(), S.Ready.end(), [](const auto& A, const auto& B) { return FState::Later(B, A); });
		// Keep unexecuted entries queued even if an exception unwinds this frame.
		while (!S.Ready.empty() && S.World.CanContinueTicking(S.World.GetCurrentLevel()))
		{
			auto Entry = std::move(S.Ready.front()); S.Ready.pop_front();
			if (!Entry.IsCurrent()) continue;
			auto Timer = Entry.Timer;
			DObject* Owner = Timer->Owner.Get();
			if (!Timer->Owner.GetKey().IsNull() && !IsTimerOwnerEligible(Owner, S.World)) { S.Remove(Timer); continue; }
			if (Timer->Interval > 0.0)
			{
				// Publish the next period before user code so self-pause preserves that remainder.
				const double Remainder = std::fmod(std::max(0.0, S.Time - Timer->Deadline), Timer->Interval);
				Timer->Deadline = S.Time + (Timer->Interval - Remainder);
				if (Timer->Deadline <= S.Time) Timer->Deadline = std::nextafter(S.Time, std::numeric_limits<double>::infinity());
			}
			// Move the callable off storage that cancellation is permitted to clear.
			auto Callback = std::move(Timer->Callback);
			try { Callback(Owner); }
			catch (...) { S.Remove(Timer); throw; }
			if (!Timer->bActive) continue;
			if (Timer->Interval == 0.0) { S.Remove(Timer); continue; }
			Timer->Callback = std::move(Callback);
			if (Timer->Revision != Entry.Revision) continue;
			if (!std::isfinite(Timer->Deadline)) { S.Remove(Timer); continue; }
			++Timer->Revision;
			S.Queue(Timer);
		}
	}

	auto FTimerManager::Reset() -> void
	{
		RequireTimerThread();
		auto& S = *State;
		for (auto& Timer : S.Slots) if (Timer) Timer->bActive = false;
		auto Retired = std::move(S.Slots);
		S.Slots.clear(); S.FreeSlots.clear(); S.Pending.clear(); S.Heap.clear(); S.Ready.clear();
		S.Time = 0.0;
	}
}
