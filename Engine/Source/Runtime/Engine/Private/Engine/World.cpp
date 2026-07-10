#include "Engine/World.h"

#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"

namespace Durin
{
	DWorld::DWorld(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DWorld::DestroyActor(AActor* Actor) -> bool
	{
		const auto It = std::find_if(Actors.begin(), Actors.end(), [Actor](const TObjectPtr<AActor>& Entry) {
			return Entry.Get() == Actor;
		});
		if (It == Actors.end())
		{
			return false;
		}

		Actors.erase(It);
		DestroyObject(Actor);
		return true;
	}

	auto DWorld::DestroyAllActors() -> void
	{
		while (!Actors.empty())
		{
			AActor* Actor = Actors.back().Get();
			Actors.pop_back();
			DestroyObject(Actor);
		}
	}

	auto DWorld::ContainsActor(const AActor* Actor) const -> bool
	{
		return Actor != nullptr && std::ranges::any_of(Actors, [Actor](const TObjectPtr<AActor>& Entry) {
				   return Entry.Get() == Actor;
			   });
	}

	auto DWorld::FindActorByName(FName Name) const -> AActor*
	{
		const auto It = std::find_if(Actors.begin(), Actors.end(), [Name](const TObjectPtr<AActor>& Entry) {
			return Entry && Entry->GetFName() == Name;
		});
		return It != Actors.end() ? It->Get() : nullptr;
	}

	auto DWorld::MakeUniqueActorName(FName RequestedName) const -> FName
	{
		if (FindActorByName(RequestedName) == nullptr)
		{
			return RequestedName;
		}

		const std::string BaseName = RequestedName.ToString();
		for (uint32 Suffix = 2;; ++Suffix)
		{
			FName Candidate(std::format("{}_{}", BaseName, Suffix));
			if (FindActorByName(Candidate) == nullptr)
			{
				return Candidate;
			}
		}
	}
} // namespace Durin
