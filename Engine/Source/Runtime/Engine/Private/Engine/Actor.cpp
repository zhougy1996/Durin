#include "Engine/Actor.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"

namespace Durin
{
	AActor::AActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		InitializeDefaults();
	}

	AActor::~AActor()
	{
		InstanceComponents.clear();
		RootComponent = nullptr;
		OwnedComponents.clear();
	}

	auto AActor::RemoveOwnedComponent(DActorComponent* Component) -> void
	{
		auto It = std::find_if(
			OwnedComponents.begin(),
			OwnedComponents.end(),
			[Component](const TObjectPtr<DActorComponent>& Entry) {
				return Entry.Get() == Component;
			}
		);
		if (It != OwnedComponents.end())
		{
			OwnedComponents.erase(It);
		}
	}

	auto AActor::RemoveInstanceComponent(DActorComponent* Component) -> void
	{
		auto It = std::find_if(
			InstanceComponents.begin(),
			InstanceComponents.end(),
			[Component](const TObjectPtr<DActorComponent>& Entry) {
				return Entry.Get() == Component;
			}
		);
		if (It != InstanceComponents.end())
		{
			InstanceComponents.erase(It);
		}
	}

	auto AActor::SetRootComponent(DSceneComponent* InRootComponent) -> bool
	{
		if (InRootComponent && InRootComponent->GetOwner() != this)
		{
			return false;
		}
		RootComponent = InRootComponent;
		return true;
	}

	auto AActor::GetActorTransform() const -> FTransform
	{
		return RootComponent ? RootComponent->GetWorldTransform() : FTransform();
	}

	auto AActor::SetActorTransform(const FTransform& InTransform) -> bool
	{
		if (!RootComponent)
		{
			return false;
		}
		RootComponent->SetWorldTransform(InTransform);
		return true;
	}

	auto AActor::AttachToActor(AActor* ParentActor, EAttachmentTransformRule Rule) -> bool
	{
		if (!ParentActor || ParentActor == this || !RootComponent || !ParentActor->GetRootComponent())
		{
			return false;
		}
		return RootComponent->AttachToComponent(ParentActor->GetRootComponent(), Rule);
	}

	auto AActor::DetachFromActor(EDetachmentTransformRule Rule) -> bool
	{
		return RootComponent && RootComponent->DetachFromComponent(Rule);
	}

	auto AActor::GetAttachParentActor() const -> AActor*
	{
		if (!RootComponent)
		{
			return nullptr;
		}
		DSceneComponent* ParentComponent = RootComponent->GetAttachParent();
		AActor* ParentActor = ParentComponent ? ParentComponent->GetOwner() : nullptr;
		return ParentActor != this ? ParentActor : nullptr;
	}

	auto AActor::InitializeDefaults() -> void
	{
	}
} // namespace Durin
