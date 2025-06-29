#pragma once
class AActor;

class ENGINE_API DActorComponent
{
public:
	DActorComponent(AActor* OwnerActor);
	virtual ~DActorComponent() = default;

	virtual FName GetDefaultName() const { return "ActorComponent"; }

	const FName& GetName() const { return ComponentName_; }
	void SetName(const FName& NewName) { ComponentName_ = NewName; }

	AActor* GetOwner() const { return OwnerActor_; }
	template<typename T> auto GetOwner() const -> T*
	{
		return dynamic_cast<T*>(OwnerActor_);
	}

private:
	AActor* OwnerActor_;

	FName ComponentName_;
};
