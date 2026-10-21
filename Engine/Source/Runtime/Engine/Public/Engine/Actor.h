#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "DObject/ObjectPtr.h"
#include "Components/SceneComponent.h"
#include "Engine/TickFunction.h"
#include "Engine/ActorConstruction.h"

#include "Actor.gen.h"

namespace Durin
{
	class DActorComponent;
	class DLevel;
	class FScopedSuspendRerunConstructionScripts;

	enum class EActorPlayState : uint8
	{
		NotBegun,
		BeginningPlay,
		Playing,
		EndingPlay
	};

	enum class EActorDestructionState : uint8
	{
		Alive,
		Requested,
		Destroying
	};

	// Owns a root scene component plus default and runtime-added component lifecycles.
	DCLASS(ScriptType)
	class AActor : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit AActor(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

		ENGINE_API ~AActor() override;

		// Human-readable editor label; legacy objects without a label use their object name.
		DFUNCTION(ScriptCallable, ScriptName = "get_actor_label")
		ENGINE_API auto GetActorLabel() const -> std::string;
		// Mechanism only. Editor callers use the Actor subsystem for admission and history.
		ENGINE_API auto SetActorLabel(const std::string& Label) -> bool;
		// Preparation before Level admission, without recording an ordinary edit.
		ENGINE_API auto SetInitialActorLabel(const std::string& Label) -> bool;

		// Internal ownership hooks used by component creation and destruction paths.
		ENGINE_API auto ResetOwnedComponents() -> void;
		ENGINE_API auto AddOwnedComponent(DActorComponent* Component) -> void;
		ENGINE_API auto RemoveOwnedComponent(DActorComponent* Component) -> void;

		// Only for internal use, should be called by DActorComponent functions
		auto RemoveInstanceComponent(DActorComponent* Component) -> void;

		FORCEINLINE auto GetRootComponent() const -> DSceneComponent* { return RootComponent; }

		ENGINE_API auto SetRootComponent(DSceneComponent* InRootComponent) -> bool;
		ENGINE_API auto AddInstanceComponent(DClass* ComponentClass, FName InName = FName()) -> DActorComponent*;
		ENGINE_API auto RenameComponent(DActorComponent* Component, FName RequestedName) -> bool;
		ENGINE_API auto DestroyInstanceComponent(DActorComponent* Component) -> bool;
		// Editor structural history retains the same object while it is absent. Removal
		// tears down registration/creation and attachments without marking it as garbage;
		// restoration publishes prepared fields without automatic attachment or root selection.
		// Gameplay, default subobjects and generated components are not admitted.
		ENGINE_API auto SetInstanceComponentPresence(DActorComponent* Component, bool bPresent, bool bRegister = true,
			size_t InstanceIndex = std::numeric_limits<size_t>::max()) -> bool;
		ENGINE_API auto IsInstanceComponent(const DActorComponent* Component) const -> bool;
		ENGINE_API auto OwnsComponent(const DActorComponent* Component) const -> bool;

		ENGINE_API auto GetActorTransform() const -> FTransform;
		ENGINE_API auto SetActorTransform(const FTransform& InTransform) -> bool;
		auto IsHidden() const -> bool { return bHidden; }
		// Assigns bHidden only; use SetActorHiddenInGame to synchronize components.
		ENGINE_API auto SetHidden(bool bInHidden) -> void;
		ENGINE_API virtual auto SetActorHiddenInGame(bool bNewHidden) -> void;

		ENGINE_API auto AttachToActor(AActor* ParentActor, EAttachmentTransformRule Rule = EAttachmentTransformRule::KeepWorld) -> bool;
		ENGINE_API auto DetachFromActor(EDetachmentTransformRule Rule = EDetachmentTransformRule::KeepWorld) -> bool;
		ENGINE_API auto GetAttachParentActor() const -> AActor*;
		ENGINE_API auto DispatchBeginPlay() -> void;
		ENGINE_API auto RouteEndPlay() -> void;
		ENGINE_API virtual auto BeginPlay() -> void;
		ENGINE_API virtual auto Tick(float DeltaSeconds) -> void;
		ENGINE_API virtual auto EndPlay() -> void;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto Modify(bool bAlwaysMarkDirty = true) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto PreEditUndo() -> void override;
		ENGINE_API auto PostEditUndo() -> void override;
		ENGINE_API auto PostEditUndo(std::shared_ptr<ITransactionObjectAnnotation> Annotation) -> void override;
		ENGINE_API auto FindOrCreateTransactionAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation> override;
		auto HasBegunPlay() const -> bool { return PlayState != EActorPlayState::NotBegun; }
		auto IsBeginningPlay() const -> bool { return PlayState == EActorPlayState::BeginningPlay; }
		auto IsEndingPlay() const -> bool { return PlayState == EActorPlayState::EndingPlay; }
		auto IsBeingDestroyed() const -> bool { return DestructionState != EActorDestructionState::Alive; }
		auto IsActorTickEnabled() const -> bool { return PrimaryActorTick.IsTickFunctionEnabled(); }
		auto SetActorTickEnabled(bool bEnabled) -> void { PrimaryActorTick.SetTickFunctionEnable(bEnabled); }
		auto GetPrimaryActorTick() -> FActorTickFunction& { return PrimaryActorTick; }
		auto GetPrimaryActorTick() const -> const FActorTickFunction& { return PrimaryActorTick; }
		// Returns every currently live component. Mutation invalidates iterators and references.
		auto GetComponents() const -> const std::vector<TObjectPtr<DActorComponent>>& { return OwnedComponents; }
		// Returns an explicit frozen entry set for mutation-capable callback boundaries.
		ENGINE_API auto GetComponentsSnapshot() const -> std::vector<TObjectPtr<DActorComponent>>;
		// Returns persistent native and instance-authored components only.
		auto GetInstanceComponents() const -> const std::vector<TObjectPtr<DActorComponent>>& { return InstanceComponents; }
		ENGINE_API auto RerunConstructionScripts(const class FComponentInstanceDataCache* InstanceData = nullptr,
			const struct FActorRootComponentReconstructionData* RootData = nullptr) -> bool;
		ENGINE_API auto ExecuteConstruction(const FTransform& Transform,
			const class FComponentInstanceDataCache* InstanceData = nullptr) -> bool;
		ENGINE_API auto DestroyConstructedComponents() -> void;
		// Stable keys supplement UE's API until template/archetype identity is available.
		ENGINE_API auto AddComponentByClass(DClass* ComponentClass, bool bManualAttachment,
			const FTransform& RelativeTransform, bool bDeferredFinish = false,
			const FActorGeneratedComponentKey& Key = {}, FName RequestedName = {}) -> DActorComponent*;
		ENGINE_API auto FinishAddComponent(DActorComponent* Component, bool bManualAttachment,
			const FTransform& RelativeTransform) -> void;
		auto IsRunningConstructionScript() const -> bool { return bRunningConstructionScript; }
		ENGINE_API auto FindGeneratedComponent(const FActorGeneratedComponentKey& Key) const -> DActorComponent*;
		ENGINE_API auto GetGeneratedComponentKey(const DActorComponent* Component, FActorGeneratedComponentKey& Key) const -> bool;
		auto GetConstructionGeneration() const -> uint64 { return NativeConstructionGeneration; }
		auto GetConstructionError() const -> const std::string& { return NativeConstructionError; }

		template<typename T>
		auto FindComponentByExactClass() const -> T*
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			for (const TObjectPtr<DActorComponent>& ComponentPtr : OwnedComponents)
			{
				DActorComponent* Component = ComponentPtr.Get();
				if (!Component)
				{
					continue;
				}
				if (Component->GetClass() == T::StaticClass())
				{
					return static_cast<T*>(Component);
				}
			}
			return nullptr;
		}

		template<typename T>
		auto FindComponentByClass() const -> T*
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			for (const TObjectPtr<DActorComponent>& ComponentPtr : OwnedComponents)
			{
				DActorComponent* Component = ComponentPtr.Get();
				if (!Component)
				{
					continue;
				}
				if (auto* CastedComponent = Cast<T>(Component))
				{
					return CastedComponent;
				}
			}
			return nullptr;
		}

		template<typename T>
		auto GetComponents(std::vector<T*>& OutComponents) const -> void
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			OutComponents.clear();
			for (const TObjectPtr<DActorComponent>& ComponentPtr : OwnedComponents)
			{
				DActorComponent* Component = ComponentPtr.Get();
				if (auto* CastedComponent = Cast<T>(Component)) OutComponents.push_back(CastedComponent);
			}
		}

		template<typename T>
		auto FindComponentsByClass() const -> std::vector<T*>
		{
			std::vector<T*> FoundComponents;
			GetComponents(FoundComponents);
			return FoundComponents;
		}

	private:
		struct FGeneratedComponentRecord
		{
			FActorGeneratedComponentKey Key;
			TObjectPtr<DActorComponent> Component;
		};

		auto InitializeDefaults() -> void;
		auto NotifyComponentsVisibilityChanged() -> void;
		auto RerunConstructionScriptsImpl(const class FComponentInstanceDataCache* InstanceData,
			const struct FActorRootComponentReconstructionData* RootData) -> bool;
		auto CompleteTransactionRestore(const class FActorTransactionAnnotation* Annotation) -> void;
		auto ValidateComponentOwnership() const -> void;
		auto MakeUniqueComponentName(FName RequestedName, const DActorComponent* IgnoredComponent = nullptr) const -> FName;
		auto RegisterTickFunction(DLevel* Level) -> void;
		auto UnregisterTickFunction() -> void;

	protected:
		DPROPERTY()
		std::string ActorLabel;

		// Persistent script/application markers, independent of mutable display labels.
		DPROPERTY(Edit, Category = "Actor")
		std::vector<FName> Tags;

		// Called exactly once after Level accepts destruction and before EndPlay or component teardown.
		ENGINE_API virtual auto OnActorDestroyed() -> void;
		// Native construction notification. Recreated components are immediately actor-owned.
		ENGINE_API virtual auto OnConstruction(const FTransform& Transform) -> void;

		template<typename T>
		auto CreateDefaultComponent(const FName& InComponentName = FName()) -> T*
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			const EObjectConstructionPurpose ComponentPurpose = IsClassDefaultObject()
				? EObjectConstructionPurpose::ClassDefaultSubobject
				: GetConstructionPurpose();
			T* Component = NewObject<T>(this, InComponentName, ComponentPurpose);
			Component->SetCreationMethod(EComponentCreationMethod::Native);
			AddOwnedComponent(Component);
			return Component;
		}

		// Root component supplies the actor transform and attachment endpoint.
		DPROPERTY()
		TObjectPtr<DSceneComponent> RootComponent;

		// Every currently live component owned by this actor; never serialized.
		DPROPERTY(Transient, NonTransactional)
		std::vector<TObjectPtr<DActorComponent>> OwnedComponents;

		// Runtime-added subset used for explicit instance-component management.
		DPROPERTY()
		std::vector<TObjectPtr<DActorComponent>> InstanceComponents;

		std::vector<FGeneratedComponentRecord> GeneratedComponents;

		DPROPERTY()
		bool bHidden = false;

		EActorPlayState PlayState = EActorPlayState::NotBegun;
		EActorDestructionState DestructionState = EActorDestructionState::Alive;
		bool bEndPlayRequested = false;
		FActorTickFunction PrimaryActorTick;
		uint64 NativeConstructionGeneration = 0;
		bool bNativeConstructionRunning = false;
		bool bRunningConstructionScript = false;
		bool bExecutingConstruction = false;
		const class FComponentInstanceDataCache* ConstructionInstanceData = nullptr;
		bool bNativeConstructionRequested = false;
		uint32 ConstructionSuspensionDepth = 0;
		std::unique_ptr<FScopedSuspendRerunConstructionScripts> EditConstructionSuspension;
		std::string NativeConstructionError;

		friend class DLevel;
		friend class FScopedSuspendRerunConstructionScripts;
	};
} // namespace Durin
