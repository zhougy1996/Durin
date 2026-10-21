#include "Engine/Actor.h"
#include "Logging/LogMacros.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Engine/TransactionAnnotations.h"
#include "ScopedSuspendRerunConstructionScripts.h"

namespace Durin
{
	AActor::AActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		PrimaryActorTick.SetTarget(this);
		InitializeDefaults();
	}

	auto AActor::Modify(bool bAlwaysMarkDirty) -> bool
	{
		bool bRecorded = Super::Modify(bAlwaysMarkDirty);
		if (RootComponent && RootComponent->GetCreationMethod() != EComponentCreationMethod::Generated)
			bRecorded = RootComponent->Modify(bAlwaysMarkDirty) || bRecorded;
		return bRecorded;
	}
	auto AActor::FindGeneratedComponent(const FActorGeneratedComponentKey& Key) const -> DActorComponent*
	{
		const auto Entry = std::ranges::find(GeneratedComponents, Key, &FGeneratedComponentRecord::Key);
		return Entry == GeneratedComponents.end() ? nullptr : Entry->Component.Get();
	}
	auto AActor::GetGeneratedComponentKey(const DActorComponent* Component, FActorGeneratedComponentKey& Key) const -> bool
	{
		const auto Entry = std::ranges::find_if(GeneratedComponents, [Component](const auto& Value) { return Value.Component.Get() == Component; });
		if (Entry == GeneratedComponents.end()) return false;
		Key = Entry->Key; return true;
	}
	auto AActor::FindOrCreateTransactionAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation>
	{
		return FActorTransactionAnnotation::CreateIfRequired(*this);
	}
	auto AActor::PreEditUndo() -> void
	{
		Super::PreEditUndo();
		if (!EditConstructionSuspension) EditConstructionSuspension = std::make_unique<FScopedSuspendRerunConstructionScripts>(*this, false);
	}
	auto AActor::PostEditUndo() -> void { CompleteTransactionRestore(nullptr); }
	auto AActor::PostEditUndo(std::shared_ptr<ITransactionObjectAnnotation> Annotation) -> void
	{
		auto State = std::dynamic_pointer_cast<FActorTransactionAnnotation>(Annotation);
		if (Annotation && !State)
		{
			EditConstructionSuspension.reset();
			throw std::runtime_error("Actor transaction annotation is incompatible.");
		}
		CompleteTransactionRestore(State.get());
	}
	auto AActor::CompleteTransactionRestore(const FActorTransactionAnnotation* Annotation) -> void
	{
		FGarbageCollectionDeferralScope DeferCollection;
		FScopedPackageDirtySuppression SuppressDirty;
		struct FCompletionScope
		{
			AActor& Actor;
			~FCompletionScope() { Actor.EditConstructionSuspension.reset(); }
		} Scope{*this};
		FScopedSuspendRerunConstructionScripts CompletionSuspension(*this, false);
		for (const auto& Component : InstanceComponents)
			if (!IsValid(Component.Get()) || Component->GetOwner() != this || Component->GetCreationMethod() != EComponentCreationMethod::Instance)
				throw std::runtime_error("Actor authored component membership cannot be restored.");
		for (const auto& Component : GetComponentsSnapshot())
		{
			if (!Component || Component->GetCreationMethod() != EComponentCreationMethod::Instance
				|| std::ranges::contains(InstanceComponents, Component)) continue;
			if (auto Result = Component->ApplyLifecycleState({}); !Result) throw std::runtime_error(Result.error());
			Component->SetOwnedByActor(false);
		}
		ResetOwnedComponents();
		if (!RerunConstructionScriptsImpl(Annotation ? &Annotation->GetComponentInstanceDataCache() : nullptr,
			Annotation ? &Annotation->GetRootReconstructionData() : nullptr)) throw std::runtime_error(NativeConstructionError.empty()
			? "Actor transaction reconstruction failed." : NativeConstructionError);
		const auto Restored = Annotation ? Annotation->Restore(*this, false) : FActorTransactionAnnotation::RestoreDefault(*this);
		if (!Restored) throw std::runtime_error(Restored.error().Message);
		if (RootComponent) RootComponent->UpdateComponentToWorld();
		NotifyComponentsVisibilityChanged();
		// Keep reconstruction suppressed while the default completion publishes its
		// full-object notification. Derived components have already settled.
		Super::PostEditUndo();
	}

	auto AActor::GetActorLabel() const -> std::string
	{
		return ActorLabel.empty() ? GetName() : ActorLabel;
	}

	auto AActor::SetActorLabel(const std::string& Label) -> bool
	{
		if (IsTemplateObject() || Label.find('\0') != std::string::npos) return false;
		const auto* Class = GetClass();
		const std::string Resolved = Label.empty()
			? (Class->GetDefaultObjectName().empty() ? Class->GetName() : Class->GetDefaultObjectName()) : Label;
		if (ActorLabel == Resolved) return true;
		ActorLabel = Resolved;
#if DURIN_WITH_EDITOR
		if (auto* Level = Cast<DLevel>(GetOuter()); Level && Level->ContainsActor(this)) Level->NotifyEditorActorHierarchyChanged();
#endif
		MarkPackageDirty();
		return true;
	}

	AActor::~AActor()
	{
		UnregisterTickFunction();
		GeneratedComponents.clear();
		InstanceComponents.clear();
		RootComponent = nullptr;
		OwnedComponents.clear();
	}

	auto AActor::AddOwnedComponent(DActorComponent* Component) -> void
	{
		if (!Component || Component->GetOwner() != this || OwnsComponent(Component)) return;
		OwnedComponents.emplace_back(Component);
		Component->SetOwnedByActor(true);
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

	auto AActor::SetInstanceComponentPresence(DActorComponent* Component, bool bPresent, bool bRegister, size_t InstanceIndex) -> bool
	{
		if (!IsValid(Component) || Component->GetOwner() != this || IsBeingDestroyed()
			|| Component->IsBeingDestroyed() || HasBegunPlay() || Component->HasBegunPlay()
			|| Component->HasAnyObjectFlags(EObjectFlags::ClassDefaultObject | EObjectFlags::DefaultSubobject)
			|| Component->GetCreationMethod() == EComponentCreationMethod::Generated) return false;
		if (bPresent)
		{
			if (OwnsComponent(Component)) return false;
			for (const auto& Existing : OwnedComponents)
				if (Existing && Existing->GetFName() == Component->GetFName()) return false;
			Component->SetCreationMethod(EComponentCreationMethod::Instance);
			DActorComponent* Next = InstanceIndex < InstanceComponents.size() ? InstanceComponents[InstanceIndex].Get() : nullptr;
			AddOwnedComponent(Component);
			InstanceComponents.emplace_back(Component);
			if (Next)
			{
				for (auto* Components : {&OwnedComponents, &InstanceComponents})
				{
					auto It = std::ranges::find_if(*Components, [Next](const auto& Entry) { return Entry.Get() == Next; });
					std::rotate(It, std::prev(Components->end()), Components->end());
				}
			}
			Component->OnComponentCreated();
			if (bRegister) Component->RegisterComponent();
		}
		else
		{
			if (!IsInstanceComponent(Component)) return false;
			if (auto* SceneComponent = Cast<DSceneComponent>(Component))
			{
				const auto Children = SceneComponent->GetAttachChildren();
				for (const auto& Child : Children)
					if (Child && Child->GetAttachParent() == SceneComponent)
						Child->DetachFromComponent(EDetachmentTransformRule::KeepWorld);
				if (SceneComponent->GetAttachParent()) SceneComponent->DetachFromComponent(EDetachmentTransformRule::KeepRelative);
				if (RootComponent == SceneComponent) SetRootComponent(nullptr);
			}
			if (auto Result = Component->ApplyLifecycleState({}); !Result) throw std::runtime_error(Result.error());
			RemoveOwnedComponent(Component);
		}
		ValidateComponentOwnership();
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
		bHidden = bInHidden;
	}

	auto AActor::SetActorHiddenInGame(bool bNewHidden) -> void
	{
		if (bHidden == bNewHidden) return;
		SetHidden(bNewHidden);
		NotifyComponentsVisibilityChanged();
		MarkPackageDirty();
	}

	auto AActor::NotifyComponentsVisibilityChanged() -> void
	{
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
		if (bNativeConstructionRequested && !bNativeConstructionRunning && !bExecutingConstruction && !IsBeingDestroyed())
			RerunConstructionScripts();
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
		if (auto* Level = Cast<DLevel>(GetOuter()); Level && Level->GetWorld())
		{
			auto& Timers = Level->GetWorld()->GetTimerManager();
			Timers.ClearAllTimersForObject(this);
			for (const auto& Component : GetComponentsSnapshot())
				if (Component) Timers.ClearAllTimersForObject(Component.Get());
		}
		EndPlay();
		if (PlayState == EActorPlayState::EndingPlay) PlayState = EActorPlayState::NotBegun;

		if (DestructionState == EActorDestructionState::Requested)
		{
			if (DLevel* Level = Cast<DLevel>(GetOuter())) Level->DestroyActor(this);
		}
		if (bNativeConstructionRequested && !bNativeConstructionRunning && !bExecutingConstruction && !IsBeingDestroyed())
			RerunConstructionScripts();
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

	auto AActor::RerunConstructionScripts(const FComponentInstanceDataCache* InstanceData,
		const FActorRootComponentReconstructionData* RootData) -> bool
	{
		if (ConstructionSuspensionDepth > 0) { bNativeConstructionRequested = true; return true; }
		return RerunConstructionScriptsImpl(InstanceData, RootData);
	}

	auto AActor::RerunConstructionScriptsImpl(const FComponentInstanceDataCache* InstanceData,
		const FActorRootComponentReconstructionData* RootData) -> bool
	{
		if (IsBeingDestroyed() || IsPendingKill()) return false;
		FGarbageCollectionDeferralScope DeferCollection;
		if (bNativeConstructionRunning || bExecutingConstruction
			|| PlayState == EActorPlayState::BeginningPlay || PlayState == EActorPlayState::EndingPlay)
		{
			bNativeConstructionRequested = true;
			return true;
		}
		bNativeConstructionRunning = true;
		struct FConstructionScope
		{
			AActor& Actor;
			~FConstructionScope() { Actor.bNativeConstructionRunning = false; Actor.bNativeConstructionRequested = false; }
		} ConstructionScope{*this};
		FScopedPackageDirtySuppression SuppressConstructionDirtying;
		bool bSucceeded = true;
		bool bStartedReconstruction = false;
		try
		{
			for (uint32 Pass = 0; Pass < 2; ++Pass)
			{
				bNativeConstructionRequested = false;
				FComponentInstanceDataCache CapturedData;
				FActorRootComponentReconstructionData CapturedRoot;
				if (!InstanceData) CapturedData = FComponentInstanceDataCache(*this);
				if (!RootData) CapturedRoot = FActorRootComponentReconstructionData(*this);
				const auto& ApplyData = InstanceData ? *InstanceData : CapturedData;
				const auto& ApplyRoot = RootData ? *RootData : CapturedRoot;
				if (auto Valid = ApplyData.Validate(this); !Valid) { NativeConstructionError = Valid.error(); bSucceeded = false; break; }
				if (auto Valid = ApplyRoot.Validate(this); !Valid) { NativeConstructionError = Valid.error(); bSucceeded = false; break; }
				const FTransform Transform = GetActorTransform();
				bStartedReconstruction = true;
				DestroyConstructedComponents();
				++NativeConstructionGeneration;
				if (!ExecuteConstruction(Transform, &ApplyData))
				{ bSucceeded = false; break; }
				if (auto Applied = ApplyRoot.ApplyToActor(*this, RootData != nullptr); !Applied)
				{ NativeConstructionError = Applied.error().Message; bSucceeded = false; break; }
				NativeConstructionError.clear();
				if (!bNativeConstructionRequested) break;
			}
		}
		catch (const std::exception& Failure) { NativeConstructionError = Failure.what(); bSucceeded = false; }
		catch (...) { NativeConstructionError = "Native component instance capture or construction threw."; bSucceeded = false; }
		if (!bSucceeded && bStartedReconstruction)
		{
			try { DestroyConstructedComponents(); }
			catch (...) { NativeConstructionError += " Component cleanup also failed; recovery is required."; }
		}
		bNativeConstructionRequested = false;
		bNativeConstructionRunning = false;
		return bSucceeded;
	}

	auto AActor::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		RerunConstructionScripts();
		// PostEditUndo dispatches a full-object edit after all transaction fields settle.
		// Direct property edits also bypass SetActorHiddenInGame and need synchronization.
		if (!Event.MemberProperty || (Event.MemberProperty->NamePrivate == FName("bHidden")
			&& Event.Origin == EPropertyChangeOrigin::Edit))
			NotifyComponentsVisibilityChanged();
	}

	auto AActor::PostLoad() -> void
	{
		std::string Error;
		Super::PostLoad();
		if (ActorLabel.empty()) ActorLabel = GetName();
		ResetOwnedComponents();
		if (RerunConstructionScripts()) return;
		Error = NativeConstructionError.empty()
			? "Actor native reconstruction failed after load or duplication."
			: NativeConstructionError;
		DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
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

	auto AActor::ResetOwnedComponents() -> void
	{
		// Outer is the ownership source; InstanceComponents alone records authored
		// instance membership. Preserve existing iteration order where possible.
		auto IsOwned = [&](const DActorComponent* Component) {
			return IsValid(Component) && !Component->IsBeingDestroyed() && Component->GetOwner() == this
				&& (Component->GetCreationMethod() != EComponentCreationMethod::Instance || IsInstanceComponent(Component));
		};
		const auto Previous = std::exchange(OwnedComponents, {});
		for (const auto& Component : Previous)
		{
			if (Component) Component->SetOwnedByActor(false);
			if (IsOwned(Component.Get())) AddOwnedComponent(Component.Get());
		}
		for (auto* Object : GDObjectArray.GetObjectsWithOuter(this, EObjectQueryScope::LiveOnly))
			if (auto* Component = Cast<DActorComponent>(Object); IsOwned(Component)) AddOwnedComponent(Component);
		for (const auto& Component : InstanceComponents)
			if (IsOwned(Component.Get())) AddOwnedComponent(Component.Get());
		// Graph allocation order is not serialized membership order. Keep native
		// and generated slots stable while restoring the authored instance order.
		auto Instance = InstanceComponents.begin();
		for (auto& Component : OwnedComponents)
			if (Component->GetCreationMethod() == EComponentCreationMethod::Instance)
			{
				check(Instance != InstanceComponents.end());
				Component = *Instance++;
			}
		check(Instance == InstanceComponents.end());
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
		for (const TObjectPtr<DActorComponent>& Component : InstanceComponents)
		{
			check(Component && Seen.contains(Component.Get()));
			check(Component->GetCreationMethod() == EComponentCreationMethod::Instance);
		}
		for (const FGeneratedComponentRecord& Entry : GeneratedComponents)
		{
			check(Entry.Component && Seen.contains(Entry.Component.Get()));
			check(Entry.Component->GetCreationMethod() == EComponentCreationMethod::Generated);

		}
#endif
	}
} // namespace Durin
