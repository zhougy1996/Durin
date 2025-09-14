#pragma once
class AActor;

class DActorComponent : public DObject
{
private:
	AActor* OwnerActor_;

	uint8 bRegistered : 1 = false;

	uint8 bHasBeenCreated : 1 = false;

	uint8 bHasBeenInitialized : 1 = false;

public:
	ENGINE_API DActorComponent(AActor* OwnerActor);

	ENGINE_API virtual ~DActorComponent() = default;

	ENGINE_API auto GetOwner() -> AActor* const { return OwnerActor_; }

	template<typename T> auto GetOwner() const -> T*
	{
		return dynamic_cast<T*>(OwnerActor_);
	}

	ENGINE_API auto RegisterComponent() -> void;

	ENGINE_API auto UnregisterComponent() -> void;

	ENGINE_API auto DestroyComponent() -> void;

	ENGINE_API virtual auto InitializeComponent() -> void;

	ENGINE_API virtual auto UninitializeComponent() -> void;

	ENGINE_API virtual auto OnRegister() -> void;

	ENGINE_API virtual auto OnUnregister() -> void;

	ENGINE_API virtual auto OnComponentCreated() -> void;

	ENGINE_API virtual auto OnComponentDestroyed() -> void;

private:
	// Call OnRegister()
	auto ExecuteRegisterEvents() -> void;

	// Call OnUnregister();
	auto ExecuteUnregisterEvents() -> void;
};
