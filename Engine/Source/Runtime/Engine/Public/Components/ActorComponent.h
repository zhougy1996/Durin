#pragma once
class AActor;

class DActorComponent
{
private:
	AActor* OwnerActor_;

	FName ComponentName_;

public:
	ENGINE_API DActorComponent(AActor* OwnerActor);

	ENGINE_API virtual ~DActorComponent() = default;

	ENGINE_API virtual FName GetDefaultName() const { return "ActorComponent"; }

	ENGINE_API auto GetName() const -> const FName& { return ComponentName_; }

	ENGINE_API auto SetName(const FName& NewName) -> void { ComponentName_ = NewName; }

	ENGINE_API auto GetOwner() -> AActor* const { return OwnerActor_; }

	template<typename T> auto GetOwner() const -> T*
	{
		return dynamic_cast<T*>(OwnerActor_);
	}

	ENGINE_API auto DestroyComponent() -> void;
};
