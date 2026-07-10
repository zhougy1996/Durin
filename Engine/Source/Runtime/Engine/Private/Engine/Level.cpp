#include "Engine/Level.h"

#include "Actors/CameraActor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"

namespace Durin
{
	DLevel::DLevel(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DLevel::DestroyActor(AActor* Actor) -> bool
	{
		const auto It = std::find_if(Actors.begin(), Actors.end(), [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; });
		if (It == Actors.end()) return false;
		if (PrimaryCameraActor.Get() == Actor) PrimaryCameraActor = nullptr;
		for (const TObjectPtr<DActorComponent>& Component : Actor->GetOwnedComponents())
		{
			if (Component && Component->IsRegistered()) Component->UnregisterComponent();
		}
		Actors.erase(It);
		DestroyObject(Actor);
		MarkPackageDirty();
		return true;
	}

	auto DLevel::DestroyAllActors() -> void
	{
		while (!Actors.empty()) DestroyActor(Actors.back().Get());
	}

	auto DLevel::ContainsActor(const AActor* Actor) const -> bool
	{
		return Actor && std::ranges::any_of(Actors, [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; });
	}

	auto DLevel::FindActorByName(FName Name) const -> AActor*
	{
		const auto It = std::find_if(Actors.begin(), Actors.end(), [Name](const TObjectPtr<AActor>& Entry) { return Entry && Entry->GetFName() == Name; });
		return It == Actors.end() ? nullptr : It->Get();
	}

	auto DLevel::SetPrimaryCameraActor(ACameraActor* Actor) -> bool
	{
		if (Actor && !ContainsActor(Actor)) return false;
		PrimaryCameraActor = Actor;
		MarkPackageDirty();
		return true;
	}

	auto DLevel::MakeUniqueActorName(FName RequestedName) const -> FName
	{
		if (!FindActorByName(RequestedName)) return RequestedName;
		const std::string BaseName = RequestedName.ToString();
		for (uint32 Suffix = 2;; ++Suffix)
		{
			FName Candidate(std::format("{}_{}", BaseName, Suffix));
			if (!FindActorByName(Candidate)) return Candidate;
		}
	}

	auto DLevel::OnActorAdded(AActor* Actor) -> void
	{
		if (!Actor || !OwningWorld) return;
		for (const TObjectPtr<DActorComponent>& Component : Actor->GetOwnedComponents())
		{
			if (Component && !Component->IsRegistered()) Component->RegisterComponent();
		}
	}

	auto DLevel::PostLoad(std::string& OutError) -> bool
	{
		std::vector<DSceneComponent*> SceneComponents;
		for (const TObjectPtr<AActor>& ActorPtr : Actors)
		{
			AActor* Actor = ActorPtr.Get();
			if (!Actor || Actor->GetOuter() != this)
			{
				OutError = "Level contains an actor outside its object graph.";
				return false;
			}
			for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
			{
				DActorComponent* Component = ComponentPtr.Get();
				if (!Component || Component->GetOuter() != Actor)
				{
					OutError = "Actor contains a component outside its object graph.";
					return false;
				}
				if (auto* SceneComponent = dynamic_cast<DSceneComponent*>(Component)) SceneComponents.push_back(SceneComponent);
			}
		}

		for (DSceneComponent* Component : SceneComponents)
		{
			Component->AttachChildren.clear();
			std::unordered_set<DSceneComponent*> Visited;
			for (DSceneComponent* Parent = Component->AttachParent.Get(); Parent; Parent = Parent->AttachParent.Get())
			{
				if (!Visited.insert(Parent).second || Parent == Component)
				{
					OutError = "Level contains a component attachment cycle.";
					return false;
				}
				if (!Parent->GetOwner() || Parent->GetOwner()->GetOuter() != this)
				{
					OutError = "Level contains a cross-level component attachment.";
					return false;
				}
			}
		}
		for (DSceneComponent* Component : SceneComponents)
		{
			if (DSceneComponent* Parent = Component->AttachParent.Get()) Parent->AttachChildren.emplace_back(Component);
		}
		for (DSceneComponent* Component : SceneComponents)
		{
			if (!Component->AttachParent) Component->UpdateComponentToWorld();
		}

		if (!PrimaryCameraActor || !ContainsActor(PrimaryCameraActor.Get()))
		{
			PrimaryCameraActor = nullptr;
			for (const TObjectPtr<AActor>& Actor : Actors)
			{
				if (auto* Camera = dynamic_cast<ACameraActor*>(Actor.Get())) { PrimaryCameraActor = Camera; break; }
			}
		}
		return true;
	}
}
