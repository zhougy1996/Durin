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
		OwnedComponents.clear();
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
		if (It != OwnedComponents.end())
		{
			if (*It) (*It)->SetOwnedByActor(false);
			OwnedComponents.erase(It);
		}
		auto GeneratedIt = std::find_if(GeneratedComponents.begin(), GeneratedComponents.end(),
			[Component](const FGeneratedComponentRecord& Entry) { return Entry.Component.Get() == Component; });
		if (GeneratedIt != GeneratedComponents.end())
		{
			if (GeneratedIt->Component) GeneratedIt->Component->SetOwnedByActor(false);
			GeneratedComponents.erase(GeneratedIt);
		}
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
		OwnedComponents.emplace_back(Component);
		InstanceComponents.emplace_back(Component);
		Component->SetOwnedByActor(true);
		Component->SetCreationMethod(EComponentCreationMethod::Instance);
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
		MarkPackageDirty();
		return Component;
	}

	auto AActor::RenameComponent(DActorComponent* Component, FName RequestedName) -> bool
	{
		if (!Component || RequestedName.IsNone() || Component->GetCreationMethod() == EComponentCreationMethod::Generated
			|| std::ranges::none_of(OwnedComponents, [Component](const TObjectPtr<DActorComponent>& Entry) { return Entry.Get() == Component; })) return false;
		Component->Rename(MakeUniqueComponentName(RequestedName, Component));
		MarkPackageDirty();
		return true;
	}

	auto AActor::MakeUniqueComponentName(FName RequestedName, const DActorComponent* IgnoredComponent) const -> FName
	{
		const std::string BaseName = RequestedName.ToString();
		FName UniqueName = RequestedName;
		for (uint32 Suffix = 2; std::ranges::any_of(GetOwnedComponents(), [&](const TObjectPtr<DActorComponent>& Entry) {
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
		return Component && std::ranges::any_of(GetOwnedComponents(), [Component](const TObjectPtr<DActorComponent>& Entry) { return Entry.Get() == Component; });
	}

	auto AActor::GetOwnedComponents() const -> std::vector<TObjectPtr<DActorComponent>>
	{
		std::vector<TObjectPtr<DActorComponent>> Result = OwnedComponents;
		Result.reserve(Result.size() + GeneratedComponents.size());
		for (const FGeneratedComponentRecord& Entry : GeneratedComponents) Result.push_back(Entry.Component);
		return Result;
	}

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
		const std::vector<TObjectPtr<DActorComponent>> Components = GetOwnedComponents();
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
		const std::vector<TObjectPtr<DActorComponent>> Components = GetOwnedComponents();
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
		const std::vector<TObjectPtr<DActorComponent>> Components = GetOwnedComponents();
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

	auto AActor::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		Super::AddReferencedObjects(Collector);
		for (FGeneratedComponentRecord& Entry : GeneratedComponents)
		{
			DObject* Object = Entry.Component.Get();
			Collector.AddReferencedObject(Object);
			Entry.Component = Cast<DActorComponent>(Object);
		}
	}

	auto AActor::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		RequestNativeReconstruction();
	}

	auto AActor::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
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
} // namespace Durin
