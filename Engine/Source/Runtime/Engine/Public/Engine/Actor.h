#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Components/SceneComponent.h"

#include "Actor.gen.h"

namespace Durin
{
	class DActorComponent;

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

		ENGINE_API auto GetActorTransform() const -> FTransform;
		ENGINE_API auto SetActorTransform(const FTransform& InTransform) -> bool;
		auto IsHidden() const -> bool { return bHidden; }
		ENGINE_API auto SetHidden(bool bInHidden) -> void;

		ENGINE_API auto AttachToActor(AActor* ParentActor, EAttachmentTransformRule Rule = EAttachmentTransformRule::KeepWorld) -> bool;
		ENGINE_API auto DetachFromActor(EDetachmentTransformRule Rule = EDetachmentTransformRule::KeepWorld) -> bool;
		ENGINE_API auto GetAttachParentActor() const -> AActor*;
		ENGINE_API virtual auto BeginPlay() -> void;
		ENGINE_API virtual auto Tick(float DeltaSeconds) -> void;
		ENGINE_API virtual auto EndPlay() -> void;
		ENGINE_API auto BeginDestroy() -> void override;
		auto HasBegunPlay() const -> bool { return bHasBegunPlay; }
		auto IsActorTickEnabled() const -> bool { return bTickEnabled; }
		auto SetActorTickEnabled(bool bEnabled) -> void { bTickEnabled = bEnabled; }
		auto GetOwnedComponents() const -> const std::vector<TObjectPtr<DActorComponent>>& { return OwnedComponents; }
		auto GetInstanceComponents() const -> const std::vector<TObjectPtr<DActorComponent>>& { return InstanceComponents; }

		template<typename T>
		auto FindComponentByStaticClass() -> T*
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			for (const TObjectPtr<DActorComponent>& ComponentPtr : OwnedComponents)
			{
				DActorComponent* Component = ComponentPtr.Get();
				if (!Component)
				{
					continue;
				}
				if (typeid(*Component) == typeid(T))
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
			for (const TObjectPtr<DActorComponent>& ComponentPtr : OwnedComponents)
			{
				DActorComponent* Component = ComponentPtr.Get();
				if (!Component)
				{
					continue;
				}
				if (auto* CastedComponent = dynamic_cast<T*>(Component))
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
			for (const TObjectPtr<DActorComponent>& ComponentPtr : OwnedComponents)
			{
				DActorComponent* Component = ComponentPtr.Get();
				if (!Component)
				{
					continue;
				}
				if (auto* CastedComponent = dynamic_cast<T*>(Component))
				{
					FoundComponents.push_back(CastedComponent);
				}
			}
			return FoundComponents;
		}

	private:
		auto InitializeDefaults() -> void;
		auto MakeUniqueComponentName(FName RequestedName, const DActorComponent* IgnoredComponent = nullptr) const -> FName;

	protected:
		template<typename T>
		auto CreateDefaultComponent(const FName& InComponentName = FName()) -> T*
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			T* Component = NewObject<T>(this, InComponentName);
			OwnedComponents.push_back(Component);
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

		DPROPERTY()
		bool bHidden = false;

		uint8 bHasBegunPlay : 1 = false;
		uint8 bTickEnabled : 1 = false;
	};
} // namespace Durin
