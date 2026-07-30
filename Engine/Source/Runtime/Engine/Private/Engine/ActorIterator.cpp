#include "Engine/ActorIterator.h"

#include "CoreGlobals.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	struct FActorIterator::FState
	{
		explicit FState(DWorld* InWorld, FActorIteratorFilter InFilter)
			: World(InWorld)
			, Filter(InFilter)
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			if (!World) return;
			Level = World->GetCurrentLevel();
			if (Level) Candidates = Level->GetActors();
		}

		auto IsEligible(size_t CandidateIndex) const -> bool
		{
			DWorld* CapturedWorld = World.Get();
			DLevel* CapturedLevel = Level.Get();
			if (!CapturedWorld || !CapturedLevel) return false;
			if (CapturedLevel->GetWorld() != CapturedWorld) return false;
			if (Filter.bRequireCurrentLevel && CapturedWorld->GetCurrentLevel() != CapturedLevel) return false;
			if (CandidateIndex >= Candidates.size()) return false;

			AActor* Actor = Candidates[CandidateIndex].Get();
			if (!Actor
				|| Actor->IsPendingKill()
				|| Actor->IsBeingDestroyed()
				|| Actor->GetOuter() != CapturedLevel)
			{
				return false;
			}
			return !Filter.ActorClass || Actor->IsA(Filter.ActorClass);
		}

		TObjectPtr<DWorld> World;
		TObjectPtr<DLevel> Level;
		std::vector<TObjectPtr<AActor>> Candidates;
		FActorIteratorFilter Filter;
	};

	FActorIterator::FActorIterator(std::shared_ptr<FState> InState, size_t InIndex)
		: State(std::move(InState))
		, Index(InIndex)
	{
		Normalize();
	}

	auto FActorIterator::Normalize() const -> void
	{
		if (!State) return;
		while (Index < State->Candidates.size() && !State->IsEligible(Index)) ++Index;
	}

	auto FActorIterator::operator*() const -> reference
	{
		Normalize();
		check(State && Index < State->Candidates.size());
		return State->Candidates[Index].Get();
	}

	auto FActorIterator::operator->() const -> pointer
	{
		return operator*();
	}

	auto FActorIterator::operator++() -> FActorIterator&
	{
		check(State && Index < State->Candidates.size());
		++Index;
		Normalize();
		return *this;
	}

	auto FActorIterator::operator++(int) -> FActorIterator
	{
		FActorIterator Previous = *this;
		++(*this);
		return Previous;
	}

	auto operator==(const FActorIterator& Left, const FActorIterator& Right) -> bool
	{
		Left.Normalize();
		Right.Normalize();
		return Left.State == Right.State && Left.Index == Right.Index;
	}

	FActorRange::FActorRange(DWorld* World, FActorIteratorFilter Filter)
		: State(std::make_shared<FActorIterator::FState>(World, Filter))
	{
	}

	auto FActorRange::begin() const -> FActorIterator
	{
		return FActorIterator(State, 0);
	}

	auto FActorRange::end() const -> FActorIterator
	{
		return FActorIterator(State, State ? State->Candidates.size() : 0);
	}

	auto FActorRange::GetInitialCandidateCount() const -> size_t
	{
		return State ? State->Candidates.size() : 0;
	}
} // namespace Durin
