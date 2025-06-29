#pragma once

class DActorComponent;
class DSceneComponent;

class ENGINE_API AActor
{
public:
	AActor();

	virtual ~AActor();

private:
	virtual auto InitializeDefaults() -> void;

protected:
	template<typename T>
	auto CreateDefaultComponent() -> T*
	{
		static_assert(std::is_base_of<DActorComponent, T>::value, "T must be derived from DActorComponent");
		TSharedPtr<T> Component = std::make_shared<T>(this);
		OwnedComponents_.push_back(std::static_pointer_cast<DActorComponent>(Component));
		return Component.get();
	}

	template<typename T>
	auto CreateDefaultComponent(const FName& ComponentName) -> T*
	{
		auto* Component = CreateDefaultComponent<T>();
		Component->SetName(ComponentName);
		return Component;
	}

	DSceneComponent* RootComponent_ = nullptr;

	TArray<TSharedPtr<DActorComponent>> OwnedComponents_;

	TArray<TSharedPtr<DActorComponent>> InstanceComponents_;

	FName Name_ = "Actor";
};