#include "Engine/ActorConstruction.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace Durin
{
	FActorConstructionContext::FActorConstructionContext(AActor& InActor, uint64 InGeneration)
		: Actor(InActor), Generation(InGeneration)
	{
		ExistingByKey.reserve(Actor.GeneratedComponents.size());
		DesiredKeys.reserve(Actor.GeneratedComponents.size());
		for (const AActor::FGeneratedComponentRecord& Existing : Actor.GeneratedComponents)
			ExistingByKey.emplace(Existing.Key, Existing.Component.Get());
		Desired.reserve(Actor.GeneratedComponents.size());
	}

	FActorConstructionContext::~FActorConstructionContext()
	{
		if (!bCommitted) RollbackCandidates();
	}

	auto FActorConstructionContext::Fail(std::string Message) -> DActorComponent*
	{
		if (Error.empty()) Error = std::move(Message);
		return nullptr;
	}

	auto FActorConstructionContext::AcquireGeneratedComponent(
		const FActorGeneratedComponentKey& Key, DClass* ExactClass, FName RequestedName) -> DActorComponent*
	{
		if (HasFailed()) return nullptr;
		if (!Key.IsValid()) return Fail("Generated component key must contain a namespace and valid GUID.");
		if (!CanConstructObjectOfClass(ExactClass, DActorComponent::StaticClass()))
			return Fail("Generated component class is not a constructible ActorComponent class.");
		if (!DesiredKeys.insert(Key).second)
			return Fail("Generated component key was acquired more than once in one construction generation.");

		if (const auto Existing = ExistingByKey.find(Key); Existing != ExistingByKey.end())
		{
			DActorComponent* Component = Existing->second;
			if (!Component || Component->IsPendingKill())
				return Fail("Generated component key refers to a retired component.");
			if (Component->GetClass() != ExactClass)
				return Fail("Generated component key was reacquired with a different exact class.");
			Desired.push_back({Key, Component, false});
			return Component;
		}

		const FName BaseName = RequestedName.IsNone()
			? FName(ExactClass->GetDefaultObjectName()) : RequestedName;
		FName UniqueName = Actor.MakeUniqueComponentName(BaseName);
		const std::string BaseText = BaseName.ToString();
		for (uint32 Suffix = 2; std::ranges::any_of(Desired, [&](const FDesiredEntry& Entry) {
			return Entry.Component && Entry.Component->GetFName() == UniqueName;
		}); ++Suffix) UniqueName = FName(std::format("{}_{}", BaseText, Suffix));
		FStaticConstructObjectParameters Params;
		Params.Class = ExactClass;
		Params.Outer = &Actor;
		Params.Name = UniqueName;
		Params.Size = ExactClass->PropertiesSize;
		Params.Purpose = EObjectConstructionPurpose::Generated;
		auto* Candidate = static_cast<DActorComponent*>(StaticConstructObject(Params));
		if (Candidate) DObjectForceRegistration(Candidate);
		if (!Candidate) return Fail("Generated component allocation failed.");
		Candidate->SetCreationMethod(EComponentCreationMethod::Generated);
		Candidate->SetOwnedByActor(true);
		Desired.push_back({Key, Candidate, true});
		return Candidate;
	}

	auto FActorConstructionContext::Commit(std::string& OutError) -> bool
	{
		if (HasFailed())
		{
			OutError = Error;
			return false;
		}
		std::vector<AActor::FGeneratedComponentRecord> Previous = Actor.GeneratedComponents;
		std::vector<AActor::FGeneratedComponentRecord> Next;
		Next.reserve(Desired.size());
		for (const FDesiredEntry& Entry : Desired)
		{
			if (!Entry.Component || Entry.Component->GetOwner() != &Actor)
			{
				OutError = "Generated component candidate does not belong to the constructed actor.";
				return false;
			}
			if (Entry.bCandidate)
			{
				if (auto* SceneComponent = Cast<DSceneComponent>(Entry.Component);
					SceneComponent && Actor.GetRootComponent() && SceneComponent != Actor.GetRootComponent())
				{
					if (!SceneComponent->AttachToComponent(Actor.GetRootComponent(),
						EAttachmentTransformRule::SnapToTarget))
					{
						OutError = "Generated scene component could not attach to the actor root.";
						return false;
					}
				}
				Entry.Component->OnComponentCreated();
			}
			Next.push_back({Entry.Key, Entry.Component});
		}

		Actor.GeneratedComponents = Next;
		auto* Level = Cast<DLevel>(Actor.GetOuter());
		DWorld* World = Level ? Level->GetWorld() : nullptr;
		const bool bActorIsInActiveWorld = World
			&& World->GetCurrentLevel() == Level
			&& Level->ContainsActor(&Actor);
		std::unordered_set<DActorComponent*> DesiredComponents;
		DesiredComponents.reserve(Desired.size());
		for (const FDesiredEntry& Entry : Desired) DesiredComponents.insert(Entry.Component);
		for (const FDesiredEntry& Entry : Desired)
		{
			if (!Entry.bCandidate) continue;
			if (bActorIsInActiveWorld) Entry.Component->RegisterComponent();
			if (Actor.PlayState == EActorPlayState::BeginningPlay
				|| Actor.PlayState == EActorPlayState::Playing) Entry.Component->DispatchBeginPlay();
		}
		for (auto It = Previous.rbegin(); It != Previous.rend(); ++It)
		{
			DActorComponent* Component = It->Component.Get();
			if (!Component || DesiredComponents.contains(Component)) continue;
			if (Component->HasBegunPlay()) Component->RouteEndPlay();
			if (Component->IsRegistered()) Component->UnregisterComponent();
			Component->DestroyComponent();
		}
		bCommitted = true;
		OutError.clear();
		return true;
	}

	auto FActorConstructionContext::RollbackCandidates() -> void
	{
		for (auto It = Desired.rbegin(); It != Desired.rend(); ++It)
		{
			if (!It->bCandidate || !It->Component || It->Component->IsPendingKill()) continue;
			if (It->Component->IsRegistered()) It->Component->UnregisterComponent();
			It->Component->DestroyComponent();
		}
	}
} // namespace Durin
