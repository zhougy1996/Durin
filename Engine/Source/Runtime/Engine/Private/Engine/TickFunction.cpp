#include "Engine/TickFunction.h"

#include "Components/ActorComponent.h"
#include "CoreGlobals.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto TickGroupIndex(ETickingGroup Group) -> size_t
		{
			return static_cast<size_t>(Group);
		}
	}

	FTickFunction::FTickFunction() = default;

	FTickFunction::~FTickFunction()
	{
		UnregisterTickFunction();
	}

	auto FTickFunction::RegisterTickFunction(DLevel* Level) -> void
	{
		if (!Level) return;
		if (Registry == &Level->TickRegistry) return;
		UnregisterTickFunction();
		Level->TickRegistry.Register(*this);
	}

	auto FTickFunction::UnregisterTickFunction() -> void
	{
		if (Registry) Registry->Unregister(*this);
	}

	auto FTickFunction::SetTickFunctionEnable(bool bInEnabled) -> void
	{
		if (bEnabled == bInEnabled) return;
		bEnabled = bInEnabled;
		if (Registry) Registry->OnEnabledChanged(*this);
	}

	auto FTickFunction::SetTickGroup(ETickingGroup Group) -> bool
	{
		if (Group == ETickingGroup::Count) return false;
		if (TickGroup == Group) return true;
		if (Registry && Registry->IsTicking()) return false;
		TickGroup = Group;
		return true;
	}

	auto FTickFunction::CancelPendingTick() -> void
	{
		if (Registry) Registry->Cancel(*this);
	}

	auto FTickFunction::NotifyEligibilityChanged() -> void
	{
		if (Registry) Registry->OnEnabledChanged(*this);
	}

	auto FActorTickFunction::CanExecute(const DLevel& Level) const -> bool
	{
		return Target
			&& !Target->IsPendingKill()
			&& Target->GetOuter() == &Level
			&& !Target->IsBeingDestroyed()
			&& Target->HasBegunPlay()
			&& !Target->IsBeginningPlay()
			&& !Target->IsEndingPlay();
	}

	auto FActorTickFunction::ExecuteTick(float DeltaSeconds) -> void
	{
		if (Target) Target->Tick(DeltaSeconds);
	}

	auto FActorComponentTickFunction::CanExecute(const DLevel& Level) const -> bool
	{
		AActor* Owner = Target ? Target->GetOwner() : nullptr;
		return Target
			&& Owner
			&& !Target->IsPendingKill()
			&& !Owner->IsPendingKill()
			&& Target->GetOuter() == Owner
			&& Owner->GetOuter() == &Level
			&& Target->IsOwnedByActor()
			&& !Target->IsBeingDestroyed()
			&& !Owner->IsBeingDestroyed()
			&& Owner->HasBegunPlay()
			&& !Owner->IsEndingPlay()
			&& Target->IsRegistered()
			&& Target->HasBegunPlay()
			&& !Target->IsBeginningPlay()
			&& !Target->IsEndingPlay();
	}

	auto FActorComponentTickFunction::ExecuteTick(float DeltaSeconds) -> void
	{
		if (Target) Target->TickComponent(DeltaSeconds);
	}

	auto FActorComponentTickFunction::GetPrerequisite() const -> FTickFunction*
	{
		AActor* Owner = Target ? Target->GetOwner() : nullptr;
		return Owner ? &Owner->GetPrimaryActorTick() : nullptr;
	}

	FTickRegistry::FTickRegistry(DLevel* InLevel)
		: Level(InLevel)
	{
	}

	FTickRegistry::~FTickRegistry()
	{
		Reset();
	}

	auto FTickRegistry::StartFrame(float InDeltaSeconds) -> void
	{
		CheckGameThreadContract();
		check(!bTicking);
		++Frame;
		if (Frame == 0) ++Frame;
		DeltaSeconds = InDeltaSeconds;
		CurrentGroup = -1;
		bTicking = true;
		for (auto& Queue : GroupQueues) Queue.clear();
		for (FTickFunction* TickFunction : RegisteredTicks)
		{
			if (TickFunction) TryQueue(*TickFunction);
		}
	}

	auto FTickRegistry::RunTickGroup(ETickingGroup Group) -> bool
	{
		CheckGameThreadContract();
		if (!bTicking || !Level || Group == ETickingGroup::Count) return false;
		const int32 GroupIndex = static_cast<int32>(Group);
		if (GroupIndex <= CurrentGroup) return false;
		CurrentGroup = GroupIndex;
		auto& Queue = GroupQueues[TickGroupIndex(Group)];
		for (size_t Index = 0; Index < Queue.size(); ++Index)
		{
			FTickFunction* TickFunction = Queue[Index];
			if (TickFunction && !ExecuteQueued(*TickFunction)) return false;
		}
		return true;
	}

	auto FTickRegistry::EndFrame() -> void
	{
		CheckGameThreadContract();
		if (!bTicking) return;
		for (auto& Queue : GroupQueues) Queue.clear();
		CurrentGroup = -1;
		DeltaSeconds = 0.0f;
		bTicking = false;
	}

	auto FTickRegistry::Reset() -> void
	{
		CheckGameThreadContract();
		for (FTickFunction* TickFunction : RegisteredTicks)
		{
			if (!TickFunction || TickFunction->Registry != this) continue;
			TickFunction->Registry = nullptr;
			TickFunction->RegistrationOrder = 0;
			TickFunction->bCancelled = true;
		}
		RegisteredTicks.clear();
		for (auto& Queue : GroupQueues) Queue.clear();
		CurrentGroup = -1;
		DeltaSeconds = 0.0f;
		bTicking = false;
	}

	auto FTickRegistry::Register(FTickFunction& TickFunction) -> void
	{
		CheckGameThreadContract();
		if (TickFunction.Registry == this) return;
		check(TickFunction.Registry == nullptr);
		TickFunction.Registry = this;
		TickFunction.RegistrationOrder = NextRegistrationOrder++;
		RegisteredTicks.push_back(&TickFunction);
		TryQueue(TickFunction);
	}

	auto FTickRegistry::Unregister(FTickFunction& TickFunction) -> void
	{
		CheckGameThreadContract();
		if (TickFunction.Registry != this) return;
		Cancel(TickFunction);
		const auto It = std::ranges::find(RegisteredTicks, &TickFunction);
		if (It != RegisteredTicks.end()) RegisteredTicks.erase(It);
		TickFunction.Registry = nullptr;
		TickFunction.RegistrationOrder = 0;
	}

	auto FTickRegistry::OnEnabledChanged(FTickFunction& TickFunction) -> void
	{
		CheckGameThreadContract();
		if (TickFunction.Registry != this) return;
		if (!TickFunction.bEnabled)
		{
			Cancel(TickFunction);
			return;
		}
		TryQueue(TickFunction);
	}

	auto FTickRegistry::Cancel(FTickFunction& TickFunction) -> void
	{
		CheckGameThreadContract();
		if (bTicking && TickFunction.QueuedFrame == Frame && !TickFunction.bExecuting)
		{
			TickFunction.bCancelled = true;
		}
	}

	auto FTickRegistry::TryQueue(FTickFunction& TickFunction) -> void
	{
		if (!bTicking
			|| TickFunction.Registry != this
			|| !TickFunction.bEnabled
			|| TickFunction.QueuedFrame == Frame
			|| !Level
			|| !TickFunction.CanExecute(*Level))
		{
			return;
		}
		const int32 GroupIndex = static_cast<int32>(TickFunction.TickGroup);
		if (GroupIndex <= CurrentGroup) return;
		TickFunction.QueuedFrame = Frame;
		TickFunction.bCancelled = false;
		GroupQueues[TickGroupIndex(TickFunction.TickGroup)].push_back(&TickFunction);
	}

	auto FTickRegistry::ExecuteQueued(FTickFunction& TickFunction) -> bool
	{
		DWorld* World = Level ? Level->GetWorld() : nullptr;
		if (!World || !World->CanContinueTicking(Level)) return false;
		if (TickFunction.Registry != this
			|| TickFunction.QueuedFrame != Frame
			|| TickFunction.ExecutedFrame == Frame
			|| TickFunction.bCancelled
			|| !TickFunction.bEnabled
			|| !TickFunction.CanExecute(*Level))
		{
			return true;
		}

		if (FTickFunction* Prerequisite = TickFunction.GetPrerequisite(); Prerequisite
			&& Prerequisite->Registry == this
			&& Prerequisite->bEnabled)
		{
			const int32 PrerequisiteGroup = static_cast<int32>(Prerequisite->TickGroup);
			if (PrerequisiteGroup > CurrentGroup)
			{
				DURIN_ERROR("Tick prerequisite belongs to a later group; dependent Tick was cancelled.");
				TickFunction.bCancelled = true;
				return true;
			}
			if (PrerequisiteGroup == CurrentGroup
				&& Prerequisite->QueuedFrame == Frame
				&& Prerequisite->ExecutedFrame != Frame)
			{
				if (TickFunction.bVisiting)
				{
					DURIN_ERROR("Tick prerequisite cycle detected; dependent Tick was cancelled.");
					TickFunction.bCancelled = true;
					return true;
				}
				TickFunction.bVisiting = true;
				const bool bContinue = ExecuteQueued(*Prerequisite);
				TickFunction.bVisiting = false;
				if (!bContinue) return false;
			}
		}

		if (TickFunction.Registry != this
			|| TickFunction.bCancelled
			|| !TickFunction.bEnabled
			|| TickFunction.ExecutedFrame == Frame
			|| !TickFunction.CanExecute(*Level))
		{
			return true;
		}

		TickFunction.ExecutedFrame = Frame;
		TickFunction.bExecuting = true;
		TickFunction.ExecuteTick(DeltaSeconds);
		TickFunction.bExecuting = false;
		return World->CanContinueTicking(Level);
	}

	auto FTickRegistry::CheckGameThreadContract() const -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
	}
} // namespace Durin
