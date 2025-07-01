#pragma once

class DActorComponent;
class DSceneComponent;

class AActor
{
public:
	ENGINE_API AActor();

	ENGINE_API virtual ~AActor();

	template<typename T>
	auto FindComponentByStaticClass() -> T*
	{
		static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
		for (auto* Component : OwnedComponents_)
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
		for (auto* Component : OwnedComponents_)
		{
			if (auto* CastedComponent = dynamic_cast<T*>(Component))
			{
				return CastedComponent;
			}
		}
		return nullptr;
	}

	template<typename T>
	auto FindComponentsByClass() -> TArray<DActorComponent*>
	{
		static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
		TArray<DActorComponent*> FoundComponents;
		for (auto* Component : OwnedComponents_)
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
		OwnedComponents_.push_back(Component);
		return Component;
	}

	template<typename T>
	auto CreateDefaultComponent(const FName& ComponentName) -> T*
	{
		auto* Component = CreateDefaultComponent<T>();
		Component->SetName(ComponentName);
		return Component;
	}

	DSceneComponent* RootComponent_ = nullptr;

	TArray<DActorComponent*> OwnedComponents_;

	TArray<DActorComponent*> InstanceComponents_;

	FName Name_ = "Actor";
};