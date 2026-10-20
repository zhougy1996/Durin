#include "Engine/Actor.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/ComponentInstanceDataCache.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace Durin
{
	auto AActor::OnConstruction(const FTransform&) -> void {}

	auto AActor::DestroyConstructedComponents() -> void
	{
		FGarbageCollectionDeferralScope DeferCollection;
		// A frozen list permits destruction callbacks to mutate live ownership.
		const auto Components = GetComponentsSnapshot();
		for (auto It = Components.rbegin(); It != Components.rend(); ++It)
			if (auto* Component = It->Get(); IsValid(Component)
				&& Component->GetCreationMethod() == EComponentCreationMethod::Generated)
			{
				Component->DestroyComponent();
				// Release its authored construction name while the retired object awaits GC.
				Component->Rename(FName(std::format("TRASH_{}_{}", Component->GetName(), FGuid::NewGuid().ToString())));
			}
	}

	auto AActor::AddComponentByClass(DClass* ComponentClass, bool bManualAttachment,
		const FTransform& RelativeTransform, bool bDeferredFinish,
		const FActorGeneratedComponentKey& Key, FName RequestedName) -> DActorComponent*
	{
		FGarbageCollectionDeferralScope DeferCollection;
		if (IsBeingDestroyed() || IsPendingKill()) throw std::runtime_error("Cannot construct a component on a retired Actor.");
		if (!CanConstructObjectOfClass(ComponentClass, DActorComponent::StaticClass()))
			throw std::runtime_error("Component class is not constructible.");
		FActorGeneratedComponentKey SourceKey = Key;
		if (!SourceKey.IsValid())
		{
			if (!Key.Namespace.IsNone() || Key.Id.IsValid()) throw std::runtime_error("Incomplete generated component source key.");
			// Like UCS matching, untemplated components use exact class and creation order.
			uint32 SerializedIndex = 0;
			for (const auto& Existing : GeneratedComponents)
				if (Existing.Component && Existing.Component->GetClass() == ComponentClass) ++SerializedIndex;
			SourceKey = {FName(std::format("Construction-{}", ComponentClass->GetQualifiedName().ToString())),
				FGuid(0, 0, 0, SerializedIndex + 1)};
		}
		if (FindGeneratedComponent(SourceKey)) throw std::runtime_error("Duplicate generated component source key.");
		FStaticConstructObjectParameters Params;
		Params.Class = ComponentClass;
		Params.Outer = this;
		Params.Name = MakeUniqueComponentName(RequestedName.IsNone() ? FName(ComponentClass->GetDefaultObjectName()) : RequestedName);
		Params.Size = ComponentClass->GetStructureSize();
		Params.Purpose = EObjectConstructionPurpose::Generated;
		auto* Component = static_cast<DActorComponent*>(StaticConstructObject(Params));
		if (!Component) throw std::runtime_error("Component allocation failed.");
		try
		{
			DObjectForceRegistration(Component);
			Component->SetCreationMethod(EComponentCreationMethod::Generated);
			if (ConstructionInstanceData)
				if (auto Result = ConstructionInstanceData->ValidateComponent(*Component, &SourceKey); !Result)
					throw std::runtime_error(Result.error());
			// Publish ownership immediately, before any creation/registration callback.
			GeneratedComponents.push_back({SourceKey, Component});
			AddOwnedComponent(Component);
			if (!bDeferredFinish) FinishAddComponent(Component, bManualAttachment, RelativeTransform);
		}
		catch (...)
		{
			if (IsValid(Component) && !Component->IsBeingDestroyed()) Component->DestroyComponent();
			throw;
		}
		return Component;
	}

	auto AActor::FinishAddComponent(DActorComponent* Component, bool bManualAttachment,
		const FTransform& RelativeTransform) -> void
	{
		FGarbageCollectionDeferralScope DeferCollection;
		if (!IsValid(Component) || Component->IsBeingDestroyed() || Component->GetOwner() != this || !OwnsComponent(Component))
			throw std::runtime_error("Cannot finish an unavailable or foreign component.");
		if (auto* Scene = Cast<DSceneComponent>(Component))
		{
			// Automatic attachment is initialization, not a construction-authored field.
			const bool bWasRunning = bRunningConstructionScript;
			bRunningConstructionScript = false;
			struct FRestoreFlag { bool& Flag; bool Previous; ~FRestoreFlag() { Flag = Previous; } } Restore{bRunningConstructionScript, bWasRunning};
			if (!bManualAttachment)
			{
				if (!GetRootComponent()) SetRootComponent(Scene);
				else if (Scene != GetRootComponent() && !Scene->GetAttachParent()
					&& !Scene->AttachToComponent(GetRootComponent(), EAttachmentTransformRule::KeepRelative))
					throw std::runtime_error("Constructed component could not attach to Actor root.");
			}
			// A deferred caller can configure its pose before finishing the component.
			if (!Scene->IsTransformModifiedByConstructionScript()) Scene->SetRelativeTransform(RelativeTransform);
		}
		if (!Component->bHasBeenCreated) Component->OnComponentCreated();
		if (!IsValid(Component) || Component->IsBeingDestroyed() || !OwnsComponent(Component) || IsBeingDestroyed())
			throw std::runtime_error("Component or Actor retired during creation.");
		auto* Level = Cast<DLevel>(GetOuter());
		auto* World = Level ? Level->GetWorld() : nullptr;
		if (World && World->GetCurrentLevel() == Level && Level->ContainsActor(this)) Component->RegisterComponent();
		if (!IsValid(Component) || Component->IsBeingDestroyed() || !OwnsComponent(Component) || IsBeingDestroyed())
			throw std::runtime_error("Component or Actor retired during registration.");
		if (PlayState == EActorPlayState::BeginningPlay || PlayState == EActorPlayState::Playing) Component->DispatchBeginPlay();
	}

	auto AActor::ExecuteConstruction(const FTransform& Transform, const FComponentInstanceDataCache* InstanceData) -> bool
	{
		if (IsBeingDestroyed() || IsPendingKill()) return false;
		if (bExecutingConstruction) throw std::runtime_error("ExecuteConstruction cannot be nested on the same Actor.");
		FGarbageCollectionDeferralScope DeferCollection;
		FScopedPackageDirtySuppression SuppressDirty;
		bExecutingConstruction = true;
		const auto* PreviousData = ConstructionInstanceData;
		ConstructionInstanceData = InstanceData;
		struct FExecutionScope
		{
			AActor& Actor; const FComponentInstanceDataCache* PreviousData;
			~FExecutionScope() { Actor.bExecutingConstruction = false; Actor.bRunningConstructionScript = false; Actor.ConstructionInstanceData = PreviousData; }
		} Scope{*this, PreviousData};
		try
		{
			if (InstanceData)
				if (auto Result = InstanceData->Validate(this); !Result) throw std::runtime_error(Result.error());
			for (const auto& Component : GetComponentsSnapshot())
				if (auto* Scene = Cast<DSceneComponent>(Component.Get())) Scene->ClearConstructionModifiedProperties();
			if (GetRootComponent()) GetRootComponent()->SetWorldTransform(Transform);
			bRunningConstructionScript = true;
			OnConstruction(Transform);
			bRunningConstructionScript = false;
			if (IsBeingDestroyed() || IsPendingKill()) throw std::runtime_error("Actor retired during construction.");
			// No SCS implementation exists yet. Native construction currently creates
			// all generated components; apply its post-construction cache once here.
			if (InstanceData)
				if (auto Result = InstanceData->ApplyToActor(*this, ECacheApplyPhase::PostUserConstructionScript); !Result)
					throw std::runtime_error(Result.error().Message);
			ValidateComponentOwnership();
			NativeConstructionError.clear();
			return true;
		}
		catch (const std::exception& Error) { NativeConstructionError = Error.what(); }
		catch (...) { NativeConstructionError = "Construction callback threw."; }
		return false;
	}
} // namespace Durin
