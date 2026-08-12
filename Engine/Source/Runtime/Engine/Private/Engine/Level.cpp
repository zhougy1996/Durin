#include "Engine/Level.h"

#include "Actors/CameraActor.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include "Engine/World.h"

namespace Durin
{
#if DURIN_WITH_EDITOR
	namespace
	{
		auto BuildEditorPickingMutation(DPrimitiveComponent* Component, bool bRetired)
			-> FEditorPickingPrimitiveMutation
		{
			FEditorPickingPrimitiveMutation Mutation;
			if (!Component) return Mutation;
			AActor* Actor = Component->GetOwner();
			Mutation.Actor = Actor;
			Mutation.Component = Component;
			Mutation.PrimitiveId = Component->GetPrimitiveSceneId();
			Mutation.RegistrationGeneration = Component->GetRegistrationGeneration();
			Mutation.bVisible = Actor && !Actor->IsHidden();
			Mutation.bRetired = bRetired;
			FBox LocalBounds;
			if (bRetired || !Component->GetEditorPickingLocalBounds(LocalBounds, Mutation.Family)) return Mutation;
			const FMatrix Transform = Component->GetRenderMatrix();
			if (!Math::IsFinite(Transform)) return Mutation;
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector3 Point(
					(Corner & 1u) ? LocalBounds.Max.x : LocalBounds.Min.x,
					(Corner & 2u) ? LocalBounds.Max.y : LocalBounds.Min.y,
					(Corner & 4u) ? LocalBounds.Max.z : LocalBounds.Min.z);
				Mutation.WorldBounds.AddPoint(FVector3(Transform * FVector4(Point, 1.0)));
			}
			return Mutation;
		}
	}
#endif

	DLevel::DLevel(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer), TickRegistry(this)
	{
	}

	DLevel::~DLevel()
	{
		TickRegistry.Reset();
	}

	auto DLevel::SpawnActor(DClass* ActorClass, FName InName) -> AActor*
	{
		return SpawnActorInternal(ActorClass, InName, true);
	}

	auto DLevel::SpawnActorDeferredPlay(DClass* ActorClass, FName InName) -> AActor*
	{
		return SpawnActorInternal(ActorClass, InName, false);
	}

	auto DLevel::SpawnActorInternal(DClass* ActorClass, FName InName, bool bDispatchBeginPlay) -> AActor*
	{
		if (OwningWorld && OwningWorld->IsEndingPlay()) return nullptr;
		if (!CanConstructObjectOfClass(ActorClass, AActor::StaticClass())) return nullptr;
		const FName RequestedName = InName.IsNone() ? FName(ActorClass->GetDefaultObjectName()) : InName;
		AActor* Actor = NewObject<AActor>(ActorClass, this, MakeUniqueActorName(RequestedName));
		if (!Actor) return nullptr;
		Actors.emplace_back(Actor);
		if (!Actor->RequestNativeReconstruction())
		{
			Actors.pop_back();
			MarkObjectHierarchyAsGarbage(Actor);
			return nullptr;
		}
#if DURIN_WITH_EDITOR
		NotifyEditorActorHierarchyChanged();
#endif
		OnActorAdded(Actor);
		if (bDispatchBeginPlay && OwningWorld && OwningWorld->HasBegunPlay()) Actor->DispatchBeginPlay();
		if (!PrimaryCameraActor)
		{
			if (auto* Camera = Cast<ACameraActor>(Actor)) PrimaryCameraActor = Camera;
		}
		MarkPackageDirty();
		return Actor;
	}

	auto DLevel::DestroyActor(AActor* Actor) -> bool
	{
		const auto It = std::find_if(Actors.begin(), Actors.end(), [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; });
		if (It == Actors.end())
		{
			return Actor
				&& Actor->GetOuter() == this
				&& Actor->DestructionState != EActorDestructionState::Alive;
		}
		if (Actor->DestructionState == EActorDestructionState::Destroying) return true;
		if (Actor->PlayState == EActorPlayState::BeginningPlay
			|| Actor->PlayState == EActorPlayState::EndingPlay)
		{
			Actor->DestructionState = EActorDestructionState::Requested;
			return true;
		}
		Actor->DestructionState = EActorDestructionState::Destroying;
		Actor->UnregisterTickFunction();
		Actor->OnActorDestroyed();
		if (OwningWorld) OwningWorld->OnActorDestroyed(Actor);
		Actor->RouteEndPlay();
		const bool bWasPrimaryCamera = PrimaryCameraActor.Get() == Actor;
		const std::vector<TObjectPtr<DActorComponent>> Components = Actor->GetOwnedComponents();
		for (const TObjectPtr<DActorComponent>& Component : Components)
		{
			if (!Component) continue;
			if (Component->IsRegistered()) Component->UnregisterComponent();
			Component->DestroyComponent();
		}
		const auto RemovalIt = std::find_if(Actors.begin(), Actors.end(), [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; });
		if (RemovalIt != Actors.end()) Actors.erase(RemovalIt);
#if DURIN_WITH_EDITOR
		NotifyEditorActorHierarchyChanged();
#endif
		if (bWasPrimaryCamera)
		{
			PrimaryCameraActor = nullptr;
			for (const TObjectPtr<AActor>& RemainingActor : Actors)
			{
				if (auto* Camera = Cast<ACameraActor>(RemainingActor.Get())) { PrimaryCameraActor = Camera; break; }
			}
		}
		MarkObjectHierarchyAsGarbage(Actor);
		MarkPackageDirty();
		return true;
	}

	auto DLevel::DestroyAllActors() -> void
	{
		const std::vector<TObjectPtr<AActor>> Snapshot = Actors;
		for (auto It = Snapshot.rbegin(); It != Snapshot.rend(); ++It)
		{
			if (*It) DestroyActor(It->Get());
		}
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

	auto DLevel::RenameActor(AActor* Actor, FName RequestedName) -> bool
	{
		if (!ContainsActor(Actor) || RequestedName.IsNone()) return false;
		Actor->Rename(MakeUniqueActorName(RequestedName, Actor));
#if DURIN_WITH_EDITOR
		NotifyEditorActorHierarchyChanged();
#endif
		MarkPackageDirty();
		return true;
	}

	auto DLevel::SetPrimaryCameraActor(ACameraActor* Actor) -> bool
	{
		if (Actor && !ContainsActor(Actor)) return false;
		PrimaryCameraActor = Actor;
		MarkPackageDirty();
		return true;
	}

	auto DLevel::MakeUniqueActorName(FName RequestedName, const AActor* IgnoredActor) const -> FName
	{
		if (AActor* Existing = FindActorByName(RequestedName); !Existing || Existing == IgnoredActor) return RequestedName;
		const std::string BaseName = RequestedName.ToString();
		for (uint32 Suffix = 2;; ++Suffix)
		{
			FName Candidate(std::format("{}_{}", BaseName, Suffix));
			if (AActor* Existing = FindActorByName(Candidate); !Existing || Existing == IgnoredActor) return Candidate;
		}
	}

	auto DLevel::OnActorAdded(AActor* Actor) -> void
	{
		if (!Actor || !OwningWorld) return;
		Actor->RegisterTickFunction(this);
		const std::vector<TObjectPtr<DActorComponent>> Components = Actor->GetOwnedComponents();
		for (const TObjectPtr<DActorComponent>& Component : Components)
		{
			if (Component
				&& !Component->IsPendingKill()
				&& Component->GetOwner() == Actor
				&& Actor->OwnsComponent(Component.Get())
				&& !Component->IsBeingDestroyed()
				&& !Component->IsRegistered())
			{
				Component->RegisterComponent();
			}
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
				Component->SetOwnedByActor(true);
				if (auto* SceneComponent = Cast<DSceneComponent>(Component)) SceneComponents.push_back(SceneComponent);
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
		for (const TObjectPtr<AActor>& Actor : Actors)
		{
			if (Actor && !Actor->RequestNativeReconstruction())
			{
				OutError = Actor->GetNativeConstructionError();
				return false;
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
				if (auto* Camera = Cast<ACameraActor>(Actor.Get())) { PrimaryCameraActor = Camera; break; }
			}
		}
#if DURIN_WITH_EDITOR
		NotifyEditorActorHierarchyChanged();
#endif
		return true;
	}

#if DURIN_WITH_EDITOR
	auto DLevel::CaptureEditorPickingPrimitiveSnapshot() const
		-> FEditorPickingPrimitiveMutationBatch
	{
		FEditorPickingPrimitiveMutationBatch Batch;
		Batch.Revision = EditorPickingPrimitiveRevision;
		Batch.bCompleteSnapshot = true;
		for (const TObjectPtr<AActor>& ActorPtr : Actors)
		{
			AActor* Actor = ActorPtr.Get();
			if (!Actor) continue;
			for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
			{
				auto* Primitive = Cast<DPrimitiveComponent>(ComponentPtr.Get());
				if (Primitive && Primitive->IsRegistered())
					Batch.Mutations.push_back(BuildEditorPickingMutation(Primitive, false));
			}
		}
		return Batch;
	}

	auto DLevel::SubscribeEditorPickingPrimitives(FEditorPickingPrimitiveObserver Observer) -> uint64
	{
		if (!Observer || bDispatchingEditorPickingMutation) return 0;
		const uint64 Subscription = NextEditorPickingObserverId++;
		EditorPickingPrimitiveObservers.emplace(Subscription, std::move(Observer));
		bDispatchingEditorPickingMutation = true;
		EditorPickingPrimitiveObservers.at(Subscription)(CaptureEditorPickingPrimitiveSnapshot());
		bDispatchingEditorPickingMutation = false;
		return Subscription;
	}

	auto DLevel::UnsubscribeEditorPickingPrimitives(uint64 Subscription) -> void
	{
		if (!Subscription) return;
		EditorPickingPrimitiveObservers.erase(Subscription);
	}

	auto DLevel::NotifyEditorPickingPrimitiveChanged(DPrimitiveComponent* Component, bool bRetired) -> void
	{
		if (!Component) return;
		require(!bDispatchingEditorPickingMutation);
		FEditorPickingPrimitiveMutationBatch Batch;
		Batch.Revision = ++EditorPickingPrimitiveRevision;
		Batch.Mutations.push_back(BuildEditorPickingMutation(Component, bRetired));
		std::vector<FEditorPickingPrimitiveObserver> Observers;
		Observers.reserve(EditorPickingPrimitiveObservers.size());
		for (const auto& [Id, Observer] : EditorPickingPrimitiveObservers)
		{
			(void)Id;
			Observers.push_back(Observer);
		}
		bDispatchingEditorPickingMutation = true;
		for (const FEditorPickingPrimitiveObserver& Observer : Observers) Observer(Batch);
		bDispatchingEditorPickingMutation = false;
	}
#endif
}
