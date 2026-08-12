#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Components/SceneComponent.h"
#include "Engine/TickFunction.h"
#include "Engine/ActorConstruction.h"

#include "Actor.gen.h"

namespace Durin
{
	class DActorComponent;
	class DLevel;

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
	DCLASS()
	class AActor : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit AActor(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

		ENGINE_API ~AActor() override;

		// Only for internal use, should be called by DActorComponent functions
		auto RemoveOwnedComponent(DActorComponent* Component) -> void;

		// Only for internal use, should be called by DActorComponent functions
		auto RemoveInstanceComponent(DActorComponent* Component) -> void;

		FORCEINLINE auto GetRootComponent() const -> DSceneComponent* { return RootComponent; }

		ENGINE_API auto SetRootComponent(DSceneComponent* InRootComponent) -> bool;
		ENGINE_API auto AddInstanceComponent(DClass* ComponentClass, FName InName = FName()) -> DActorComponent*;
		ENGINE_API auto RenameComponent(DActorComponent* Component, FName RequestedName) -> bool;
		ENGINE_API auto DestroyInstanceComponent(DActorComponent* Component) -> bool;
		ENGINE_API auto IsInstanceComponent(const DActorComponent* Component) const -> bool;
		ENGINE_API auto OwnsComponent(const DActorComponent* Component) const -> bool;

		ENGINE_API auto GetActorTransform() const -> FTransform;
		ENGINE_API auto SetActorTransform(const FTransform& InTransform) -> bool;
		auto IsHidden() const -> bool { return bHidden; }
		ENGINE_API auto SetHidden(bool bInHidden) -> void;

		ENGINE_API auto AttachToActor(AActor* ParentActor, EAttachmentTransformRule Rule = EAttachmentTransformRule::KeepWorld) -> bool;
		ENGINE_API auto DetachFromActor(EDetachmentTransformRule Rule = EDetachmentTransformRule::KeepWorld) -> bool;
		ENGINE_API auto GetAttachParentActor() const -> AActor*;
		ENGINE_API auto DispatchBeginPlay() -> void;
		ENGINE_API auto RouteEndPlay() -> void;
		ENGINE_API virtual auto BeginPlay() -> void;
		ENGINE_API virtual auto Tick(float DeltaSeconds) -> void;
		ENGINE_API virtual auto EndPlay() -> void;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		auto HasBegunPlay() const -> bool { return PlayState != EActorPlayState::NotBegun; }
		auto IsBeginningPlay() const -> bool { return PlayState == EActorPlayState::BeginningPlay; }
		auto IsEndingPlay() const -> bool { return PlayState == EActorPlayState::EndingPlay; }
		auto IsBeingDestroyed() const -> bool { return DestructionState != EActorDestructionState::Alive; }
		auto IsActorTickEnabled() const -> bool { return PrimaryActorTick.IsTickFunctionEnabled(); }
		auto SetActorTickEnabled(bool bEnabled) -> void { PrimaryActorTick.SetTickFunctionEnable(bEnabled); }
		auto GetPrimaryActorTick() -> FActorTickFunction& { return PrimaryActorTick; }
		auto GetPrimaryActorTick() const -> const FActorTickFunction& { return PrimaryActorTick; }
		// Returns persistent native-default and instance-authored components only.
		auto GetAuthoredComponents() const -> const std::vector<TObjectPtr<DActorComponent>>& { return OwnedComponents; }
		// Returns a stable snapshot of every currently live authored and generated component.
		ENGINE_API auto GetOwnedComponents() const -> std::vector<TObjectPtr<DActorComponent>>;
		auto GetInstanceComponents() const -> const std::vector<TObjectPtr<DActorComponent>>& { return InstanceComponents; }
		ENGINE_API auto RequestNativeReconstruction() -> bool;
		auto GetNativeConstructionGeneration() const -> uint64 { return NativeConstructionGeneration; }
		auto GetNativeConstructionError() const -> const std::string& { return NativeConstructionError; }

		template<typename T>
		auto FindComponentByStaticClass() -> T*
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			for (const TObjectPtr<DActorComponent>& ComponentPtr : GetOwnedComponents())
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
		auto FindComponentByClass() -> T*
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			for (const TObjectPtr<DActorComponent>& ComponentPtr : GetOwnedComponents())
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
		auto FindComponentsByClass() -> std::vector<DActorComponent*>
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			std::vector<DActorComponent*> FoundComponents;
			for (const TObjectPtr<DActorComponent>& ComponentPtr : GetOwnedComponents())
			{
				DActorComponent* Component = ComponentPtr.Get();
				if (!Component)
				{
					continue;
				}
				if (auto* CastedComponent = Cast<T>(Component))
				{
					FoundComponents.push_back(CastedComponent);
				}
			}
			return FoundComponents;
		}

	private:
		struct FGeneratedComponentRecord
		{
			FActorGeneratedComponentKey Key;
			TObjectPtr<DActorComponent> Component;
		};

		auto InitializeDefaults() -> void;
		auto MakeUniqueComponentName(FName RequestedName, const DActorComponent* IgnoredComponent = nullptr) const -> FName;
		auto RegisterTickFunction(DLevel* Level) -> void;
		auto UnregisterTickFunction() -> void;

	protected:
		// Called exactly once after Level accepts destruction and before EndPlay or component teardown.
		ENGINE_API virtual auto OnActorDestroyed() -> void;
		// Declares one complete desired generated set for a synchronous construction generation.
		ENGINE_API virtual auto OnNativeConstruct(FActorConstructionContext& Context,
			std::string& OutError) -> bool;

		template<typename T>
		auto CreateDefaultComponent(const FName& InComponentName = FName()) -> T*
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			const EObjectConstructionPurpose ComponentPurpose = IsClassDefaultObject()
				? EObjectConstructionPurpose::ClassDefaultSubobject
				: GetConstructionPurpose();
			T* Component = NewObject<T>(this, InComponentName, ComponentPurpose);
			OwnedComponents.push_back(Component);
			Component->SetOwnedByActor(true);
			Component->SetCreationMethod(EComponentCreationMethod::NativeDefault);
			return Component;
		}

		// Root component supplies the actor transform and attachment endpoint.
		DPROPERTY()
		TObjectPtr<DSceneComponent> RootComponent;

		// All components structurally owned by this actor, including default components.
		DPROPERTY()
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
		bool bNativeConstructionRequested = false;
		std::string NativeConstructionError;

		friend class DLevel;
		friend class FActorConstructionContext;
	};
} // namespace Durin
