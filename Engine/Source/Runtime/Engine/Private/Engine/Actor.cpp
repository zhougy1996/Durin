#include "Engine/Actor.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Level.h"

namespace Durin
{
	AActor::AActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		PrimaryActorTick.SetTarget(this);
		InitializeDefaults();
	}

	AActor::~AActor()
	{
		UnregisterTickFunction();
		GeneratedComponents.clear();
		InstanceComponents.clear();
		RootComponent = nullptr;
		AuthoredComponents.clear();
		OwnedComponents.clear();
	}

	auto AActor::AddOwnedComponent(DActorComponent* Component) -> void
	{
		if (!Component || Component->GetOwner() != this || OwnsComponent(Component)) return;
		OwnedComponents.emplace_back(Component);
		Component->SetOwnedByActor(true);
	}

	auto AActor::AddAuthoredComponent(DActorComponent* Component) -> void
	{
		if (!Component || std::ranges::any_of(AuthoredComponents,
			[Component](const TObjectPtr<DActorComponent>& Entry) { return Entry.Get() == Component; })) return;
		AuthoredComponents.emplace_back(Component);
	}

	auto AActor::RemoveOwnedComponent(DActorComponent* Component) -> void
	{
		auto It = std::find_if(
			OwnedComponents.begin(),
			OwnedComponents.end(),
			[Component](const TObjectPtr<DActorComponent>& Entry) {
				return Entry.Get() == Component;
			}
		);
		if (It != OwnedComponents.end()) OwnedComponents.erase(It);
		auto AuthoredIt = std::find_if(AuthoredComponents.begin(), AuthoredComponents.end(),
			[Component](const TObjectPtr<DActorComponent>& Entry) { return Entry.Get() == Component; });
		if (AuthoredIt != AuthoredComponents.end()) AuthoredComponents.erase(AuthoredIt);
		RemoveInstanceComponent(Component);
		auto GeneratedIt = std::find_if(GeneratedComponents.begin(), GeneratedComponents.end(),
			[Component](const FGeneratedComponentRecord& Entry) { return Entry.Component.Get() == Component; });
		if (GeneratedIt != GeneratedComponents.end())
		{
			GeneratedComponents.erase(GeneratedIt);
		}
		if (Component) Component->SetOwnedByActor(false);
		ValidateComponentOwnership();
	}

	auto AActor::RemoveInstanceComponent(DActorComponent* Component) -> void
	{
		auto It = std::find_if(
			InstanceComponents.begin(),
			InstanceComponents.end(),
			[Component](const TObjectPtr<DActorComponent>& Entry) {
				return Entry.Get() == Component;
			}
		);
		if (It != InstanceComponents.end())
		{
			InstanceComponents.erase(It);
		}
	}

	auto AActor::SetRootComponent(DSceneComponent* InRootComponent) -> bool
	{
		if (InRootComponent && InRootComponent->GetOwner() != this)
		{
			return false;
		}
#if DURIN_WITH_EDITOR
		AActor* PreviousParentActor = GetAttachParentActor();
#endif
		RootComponent = InRootComponent;
#if DURIN_WITH_EDITOR
		if (PreviousParentActor != GetAttachParentActor())
		{
			if (auto* Level = Cast<DLevel>(GetOuter())) Level->NotifyEditorActorHierarchyChanged();
		}
#endif
		return true;
	}

	auto AActor::AddInstanceComponent(DClass* ComponentClass, FName InName) -> DActorComponent*
	{
		if (!CanConstructObjectOfClass(ComponentClass, DActorComponent::StaticClass())) return nullptr;
		const FName RequestedName = InName.IsNone() ? FName(ComponentClass->GetDefaultObjectName()) : InName;
		const FName UniqueName = MakeUniqueComponentName(RequestedName);
		DActorComponent* Component = NewObject<DActorComponent>(ComponentClass, this, UniqueName);
		if (!Component) return nullptr;
		Component->SetCreationMethod(EComponentCreationMethod::Instance);
		AddAuthoredComponent(Component);
		AddOwnedComponent(Component);
		InstanceComponents.emplace_back(Component);
		Component->OnComponentCreated();
		if (auto* SceneComponent = Cast<DSceneComponent>(Component))
		{
			if (RootComponent) SceneComponent->AttachToComponent(RootComponent.Get(), EAttachmentTransformRule::KeepWorld);
			else SetRootComponent(SceneComponent);
		}
		Component->RegisterComponent();
		if (PlayState == EActorPlayState::BeginningPlay || PlayState == EActorPlayState::Playing)
		{
			Component->DispatchBeginPlay();
		}
		ValidateComponentOwnership();
		MarkPackageDirty();
		return Component;
	}

	auto AActor::RenameComponent(DActorComponent* Component, FName RequestedName) -> bool
	{
		if (!Component || RequestedName.IsNone() || Component->GetCreationMethod() == EComponentCreationMethod::Generated
			|| !OwnsComponent(Component)) return false;
		Component->Rename(MakeUniqueComponentName(RequestedName, Component));
		MarkPackageDirty();
		return true;
	}

	auto AActor::MakeUniqueComponentName(FName RequestedName, const DActorComponent* IgnoredComponent) const -> FName
	{
		const std::string BaseName = RequestedName.ToString();
		FName UniqueName = RequestedName;
		for (uint32 Suffix = 2; std::ranges::any_of(OwnedComponents, [&](const TObjectPtr<DActorComponent>& Entry) {
				 return Entry && Entry.Get() != IgnoredComponent && Entry->GetFName() == UniqueName;
			 }); ++Suffix)
		{
			UniqueName = FName(std::format("{}_{}", BaseName, Suffix));
		}
		return UniqueName;
	}

	auto AActor::DestroyInstanceComponent(DActorComponent* Component) -> bool
	{
		if (!IsInstanceComponent(Component)) return false;
		Component->DestroyComponent();
		MarkPackageDirty();
		return true;
	}

	auto AActor::IsInstanceComponent(const DActorComponent* Component) const -> bool
	{
		return Component && std::ranges::any_of(InstanceComponents, [Component](const TObjectPtr<DActorComponent>& Entry) { return Entry.Get() == Component; });
	}

	auto AActor::OwnsComponent(const DActorComponent* Component) const -> bool
	{
		return Component && std::ranges::any_of(OwnedComponents, [Component](const TObjectPtr<DActorComponent>& Entry) { return Entry.Get() == Component; });
	}

	auto AActor::GetComponentsSnapshot() const -> std::vector<TObjectPtr<DActorComponent>>
	{
		return OwnedComponents;
	}

	auto AActor::GetOwnedComponents() const -> std::vector<TObjectPtr<DActorComponent>> { return GetComponentsSnapshot(); }

	auto AActor::GetActorTransform() const -> FTransform
	{
		return RootComponent ? RootComponent->GetWorldTransform() : FTransform();
	}

	auto AActor::SetActorTransform(const FTransform& InTransform) -> bool
	{
		if (!RootComponent)
		{
			return false;
		}
		RootComponent->SetWorldTransform(InTransform);
		return true;
	}

	auto AActor::SetHidden(bool bInHidden) -> void
	{
		if (bHidden == bInHidden) return;
		bHidden = bInHidden;
		const std::vector<TObjectPtr<DActorComponent>> Components = GetComponentsSnapshot();
		for (const TObjectPtr<DActorComponent>& Component : Components)
		{
			if (Component
				&& !Component->IsPendingKill()
				&& Component->GetOwner() == this
				&& OwnsComponent(Component.Get()))
			{
				Component->OnOwnerVisibilityChanged();
			}
		}
		MarkPackageDirty();
	}

	auto AActor::AttachToActor(AActor* ParentActor, EAttachmentTransformRule Rule) -> bool
	{
		if (!ParentActor || ParentActor == this || !RootComponent || !ParentActor->GetRootComponent())
		{
			return false;
		}
		return RootComponent->AttachToComponent(ParentActor->GetRootComponent(), Rule);
	}

	auto AActor::DetachFromActor(EDetachmentTransformRule Rule) -> bool
	{
		return RootComponent && RootComponent->DetachFromComponent(Rule);
	}

	auto AActor::GetAttachParentActor() const -> AActor*
	{
		if (!RootComponent)
		{
			return nullptr;
		}
		DSceneComponent* ParentComponent = RootComponent->GetAttachParent();
		AActor* ParentActor = ParentComponent ? ParentComponent->GetOwner() : nullptr;
		return ParentActor != this ? ParentActor : nullptr;
	}

	auto AActor::DispatchBeginPlay() -> void
	{
		if (PlayState != EActorPlayState::NotBegun
			|| DestructionState != EActorDestructionState::Alive
			|| IsPendingKill())
		{
			return;
		}

		PlayState = EActorPlayState::BeginningPlay;
		BeginPlay();
		if (PlayState == EActorPlayState::BeginningPlay)
		{
			PlayState = EActorPlayState::Playing;
			PrimaryActorTick.NotifyEligibilityChanged();
		}

		if (bEndPlayRequested)
		{
			bEndPlayRequested = false;
			RouteEndPlay();
		}

		if (DestructionState == EActorDestructionState::Requested)
		{
			if (DLevel* Level = Cast<DLevel>(GetOuter())) Level->DestroyActor(this);
		}
	}

	auto AActor::RouteEndPlay() -> void
	{
		if (PlayState == EActorPlayState::NotBegun || PlayState == EActorPlayState::EndingPlay) return;
		if (PlayState == EActorPlayState::BeginningPlay)
		{
			bEndPlayRequested = true;
			return;
		}

		PrimaryActorTick.CancelPendingTick();
		PlayState = EActorPlayState::EndingPlay;
		EndPlay();
		if (PlayState == EActorPlayState::EndingPlay) PlayState = EActorPlayState::NotBegun;

		if (DestructionState == EActorDestructionState::Requested)
		{
			if (DLevel* Level = Cast<DLevel>(GetOuter())) Level->DestroyActor(this);
		}
	}

	auto AActor::BeginPlay() -> void
	{
		const std::vector<TObjectPtr<DActorComponent>> Components = GetComponentsSnapshot();
		for (const TObjectPtr<DActorComponent>& Component : Components)
		{
			if (Component
				&& !Component->IsPendingKill()
				&& Component->GetOwner() == this
				&& OwnsComponent(Component.Get())
				&& Component->IsRegistered()
				&& !Component->IsBeingDestroyed()
				&& !Component->HasBegunPlay())
			{
				Component->DispatchBeginPlay();
			}
		}
	}

	auto AActor::Tick(float DeltaSeconds) -> void
	{
		(void)DeltaSeconds;
	}

	auto AActor::EndPlay() -> void
	{
		const std::vector<TObjectPtr<DActorComponent>> Components = GetComponentsSnapshot();
		for (auto It = Components.rbegin(); It != Components.rend(); ++It)
		{
			if (*It
				&& !(*It)->IsPendingKill()
				&& (*It)->GetOwner() == this
				&& OwnsComponent(It->Get())
				&& !(*It)->IsBeingDestroyed()
				&& (*It)->HasBegunPlay())
			{
				(*It)->RouteEndPlay();
			}
		}
	}

	auto AActor::OnActorDestroyed() -> void
	{
	}

	auto AActor::OnNativeConstruct(FActorConstructionContext& Context, std::string& OutError) -> bool
	{
		(void)Context;
		OutError.clear();
		return true;
	}

	auto AActor::RequestNativeReconstruction() -> bool
	{
		if (IsBeingDestroyed() || IsPendingKill()) return false;
		if (bNativeConstructionRunning)
		{
			bNativeConstructionRequested = true;
			return true;
		}
		bNativeConstructionRunning = true;
		FScopedPackageDirtySuppression SuppressConstructionDirtying;
		bool bSucceeded = true;
		for (uint32 Pass = 0; Pass < 2; ++Pass)
		{
			bNativeConstructionRequested = false;
			FActorConstructionContext Context(*this, ++NativeConstructionGeneration);
			std::string Error;
			if (!OnNativeConstruct(Context, Error) || Context.HasFailed() || !Context.Commit(Error))
			{
				NativeConstructionError = Context.HasFailed() ? Context.GetError() : Error;
				bSucceeded = false;
				break;
			}
			NativeConstructionError.clear();
			if (!bNativeConstructionRequested) break;
		}
		bNativeConstructionRequested = false;
		bNativeConstructionRunning = false;
		return bSucceeded;
	}

	auto AActor::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		RequestNativeReconstruction();
	}

	auto AActor::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		RebuildOwnedComponentsFromAuthored();
		if (RequestNativeReconstruction()) return true;
		OutError = NativeConstructionError.empty()
			? "Actor native reconstruction failed after load or duplication."
			: NativeConstructionError;
		return false;
	}

	auto AActor::BeginDestroy() -> void
	{
		UnregisterTickFunction();
		RouteEndPlay();
		Super::BeginDestroy();
	}

	auto AActor::RegisterTickFunction(DLevel* Level) -> void
	{
		PrimaryActorTick.RegisterTickFunction(Level);
	}

	auto AActor::UnregisterTickFunction() -> void
	{
		PrimaryActorTick.UnregisterTickFunction();
	}

	auto AActor::InitializeDefaults() -> void
	{
	}

	auto AActor::RebuildOwnedComponentsFromAuthored() -> void
	{
		OwnedComponents.clear();
		for (const TObjectPtr<DActorComponent>& Component : AuthoredComponents)
		{
			if (!Component || Component->GetOwner() != this) continue;
			AddOwnedComponent(Component.Get());
		}
		// A duplicated authored component may complete PostLoad before its Actor and
		// synchronously request native reconstruction. Preserve those already-keyed
		// derived components so the Actor's final reconstruction can reuse them.
		for (const FGeneratedComponentRecord& Entry : GeneratedComponents)
		{
			if (!Entry.Component || Entry.Component->GetOwner() != this) continue;
			AddOwnedComponent(Entry.Component.Get());
		}
		ValidateComponentOwnership();
	}

	auto AActor::ValidateComponentOwnership() const -> void
	{
#if DO_CHECK
		std::unordered_set<const DActorComponent*> Seen;
		for (const TObjectPtr<DActorComponent>& Component : OwnedComponents)
		{
			check(Component && Component->GetOwner() == this && Component->IsOwnedByActor());
			check(Seen.insert(Component.Get()).second);
		}
		for (const TObjectPtr<DActorComponent>& Component : AuthoredComponents)
		{
			check(Component && Seen.contains(Component.Get()));
			check(Component->GetCreationMethod() != EComponentCreationMethod::Generated);
		}
		for (const TObjectPtr<DActorComponent>& Component : InstanceComponents)
		{
			check(Component && std::ranges::any_of(AuthoredComponents,
				[&](const TObjectPtr<DActorComponent>& Entry) { return Entry.Get() == Component.Get(); }));
			check(Component->GetCreationMethod() == EComponentCreationMethod::Instance);
		}
		for (const FGeneratedComponentRecord& Entry : GeneratedComponents)
		{
			check(Entry.Component && Seen.contains(Entry.Component.Get()));
			check(Entry.Component->GetCreationMethod() == EComponentCreationMethod::Generated);
			check(std::ranges::none_of(AuthoredComponents,
				[&](const TObjectPtr<DActorComponent>& Component) { return Component.Get() == Entry.Component.Get(); }));
		}
#endif
	}
} // namespace Durin
