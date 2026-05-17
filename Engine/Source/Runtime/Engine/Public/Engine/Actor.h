#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Components/ActorComponent.h"

#include "Actor.gen.h"

namespace Durin
{
	class DActorComponent;
	class DSceneComponent;

	DCLASS()
	class AActor : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API AActor();

		ENGINE_API AActor(const FObjectInitializer& ObjectInitializer);

		ENGINE_API virtual ~AActor();

		// Only for internal use, should be called by DActorComponent functions
		auto RemoveOwnedComponent(DActorComponent* Component) -> void;

		// Only for internal use, should be called by DActorComponent functions
		auto RemoveInstanceComponent(DActorComponent* Component) -> void;

		FORCEINLINE auto GetRootComponent() const -> DSceneComponent* { return RootComponent; }

		FORCEINLINE auto SetRootComponent(DSceneComponent* InRootComponent) -> void { RootComponent = InRootComponent; }

		template<typename T>
		auto FindComponentByStaticClass() -> T*
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			for (auto* Component : OwnedComponents)
			{
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
			for (auto* Component : OwnedComponents)
			{
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
			for (auto* Component : OwnedComponents)
			{
				if (auto* CastedComponent = dynamic_cast<T*>(Component))
				{
					FoundComponents.push_back(CastedComponent);
				}
			}
			return FoundComponents;
		}

	private:
		auto InitializeDefaults() -> void;

	protected:
		template<typename T>
		auto CreateDefaultComponent() -> T*
		{
			static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
			T* Component = new T(this);
			OwnedComponents.push_back(Component);
			return Component;
		}

		template<typename T>
		auto CreateDefaultComponent(const FName& InComponentName) -> T*
		{
			auto* Component = CreateDefaultComponent<T>();
			Component->Rename(InComponentName);
			return Component;
		}

		DSceneComponent* RootComponent = nullptr;

		std::vector<DActorComponent*> OwnedComponents;

		std::vector<DActorComponent*> InstanceComponents;
	};
}