#include "Engine/Actor.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#if DURIN_WITH_EDITOR
#include "Engine/Level.h"
#endif

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
#if DURIN_WITH_EDITOR
		AActor* PreviousParentActor = GetAttachParentActor();
#endif
		RootComponent = InRootComponent;
#if DURIN_WITH_EDITOR
		if (PreviousParentActor != GetAttachParentActor())
		{
			if (auto* Level = Cast<DLevel>(GetOuter())) Level->NotifyEditorActorHierarchyChanged();
		}
#endif
		return true;
	}

	auto AActor::AddInstanceComponent(DClass* ComponentClass, FName InName) -> DActorComponent*
	{
		if (!CanConstructObjectOfClass(ComponentClass, DActorComponent::StaticClass())) return nullptr;
		const FName RequestedName = InName.IsNone() ? FName(ComponentClass->GetDefaultObjectName()) : InName;
		const FName UniqueName = MakeUniqueComponentName(RequestedName);
		DActorComponent* Component = NewObject<DActorComponent>(ComponentClass, this, UniqueName);
		if (!Component) return nullptr;
		OwnedComponents.emplace_back(Component);
		InstanceComponents.emplace_back(Component);
		Component->OnComponentCreated();
		if (auto* SceneComponent = Cast<DSceneComponent>(Component))
		{
			if (RootComponent) SceneComponent->AttachToComponent(RootComponent.Get(), EAttachmentTransformRule::KeepWorld);
			else SetRootComponent(SceneComponent);
		}
		Component->RegisterComponent();
		if (bHasBegunPlay) Component->BeginPlay();
		MarkPackageDirty();
		return Component;
	}

	auto AActor::RenameComponent(DActorComponent* Component, FName RequestedName) -> bool
	{
		if (!Component || RequestedName.IsNone() || std::ranges::none_of(OwnedComponents, [Component](const TObjectPtr<DActorComponent>& Entry) { return Entry.Get() == Component; })) return false;
		Component->Rename(MakeUniqueComponentName(RequestedName, Component));
		MarkPackageDirty();
		return true;
	}

	auto AActor::MakeUniqueComponentName(FName RequestedName, const DActorComponent* IgnoredComponent) const -> FName
	{
		const std::string BaseName = RequestedName.ToString();
		FName UniqueName = RequestedName;
		for (uint32 Suffix = 2; std::ranges::any_of(OwnedComponents, [&](const TObjectPtr<DActorComponent>& Entry) {
				 return Entry && Entry.Get() != IgnoredComponent && Entry->GetFName() == UniqueName;
			 }); ++Suffix)
		{
			UniqueName = FName(std::format("{}_{}", BaseName, Suffix));
		}
		return UniqueName;
	}

	auto AActor::DestroyInstanceComponent(DActorComponent* Component) -> bool
	{
		if (!IsInstanceComponent(Component)) return false;
		Component->DestroyComponent();
		MarkPackageDirty();
		return true;
	}

	auto AActor::IsInstanceComponent(const DActorComponent* Component) const -> bool
	{
		return Component && std::ranges::any_of(InstanceComponents, [Component](const TObjectPtr<DActorComponent>& Entry) { return Entry.Get() == Component; });
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

	auto AActor::SetHidden(bool bInHidden) -> void
	{
		if (bHidden == bInHidden) return;
		bHidden = bInHidden;
		for (const TObjectPtr<DActorComponent>& Component : OwnedComponents)
		{
			if (Component) Component->OnOwnerVisibilityChanged();
		}
		MarkPackageDirty();
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

	auto AActor::BeginPlay() -> void
	{
		check(!bHasBegunPlay);
		bHasBegunPlay = true;
		for (const TObjectPtr<DActorComponent>& Component : OwnedComponents)
		{
			if (Component && Component->IsRegistered() && !Component->HasBegunPlay()) Component->BeginPlay();
		}
	}

	auto AActor::Tick(float DeltaSeconds) -> void
	{
		for (const TObjectPtr<DActorComponent>& Component : OwnedComponents)
		{
			if (Component && Component->HasBegunPlay() && Component->IsComponentTickEnabled()) Component->TickComponent(DeltaSeconds);
		}
	}

	auto AActor::EndPlay() -> void
	{
		if (!bHasBegunPlay) return;
		for (auto It = OwnedComponents.rbegin(); It != OwnedComponents.rend(); ++It)
		{
			if (*It && (*It)->HasBegunPlay()) (*It)->EndPlay();
		}
		bHasBegunPlay = false;
	}

	auto AActor::BeginDestroy() -> void
	{
		EndPlay();
		Super::BeginDestroy();
	}

	auto AActor::InitializeDefaults() -> void
	{
	}
} // namespace Durin
