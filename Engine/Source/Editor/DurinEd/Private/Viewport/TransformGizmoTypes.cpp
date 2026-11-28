#include "Viewport/TransformGizmoTypes.h"
#include "Components/SceneComponent.h"
#include "Engine/Actor.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin::LevelEditor
{
	DSceneComponent* FTransformComponentBinding::GetDataOwner() const { return Component.Get(); }

	bool FTransformComponentBinding::IsValid(uint64 HistoryId) const
	{
		auto* Bound = Component.Get();
		if (!Bound) return false;
		if (!RootOwner.GetKey().IsNull())
		{
			auto* Actor = RootOwner.Get();
			if (!Actor || Actor->IsHidden() || Actor->GetRootComponent() != Bound) return false;
		}
		return !Options.Validate || Options.Validate(HistoryId);
	}

	FTransform FTransformComponentBinding::GetTransform() const
	{
		if (Getter) return Getter();
		auto* Bound = Component.Get();
		return Bound ? Bound->GetWorldTransform() : FTransform{};
	}

	bool FTransformComponentBinding::SetTransform(const FTransform& Value, FTransformGizmoWriteContext Context) const
	{
		if (!IsValid(Context.ReplayHistoryId)) return false;
		if (Setter) return Setter(Value, Context);
		Component.Get()->SetWorldTransform(Value);
		return true;
	}

	FQuat FTransformComponentBinding::GetParentRotation() const
	{
		auto* Parent = !Getter && Component.Get() ? Component.Get()->GetAttachParent() : nullptr;
		return Parent ? Parent->GetWorldRotation() : FQuatConstants::Identity;
	}

	DPackage* FTransformComponentBinding::GetPackage() const
	{
		return Component.Get() ? Component.Get()->GetPackage() : nullptr;
	}

	void FTransformComponentBinding::AddReferencedObjects(FReferenceCollector& Collector) const
	{
		DObject* Owner = RootOwner.Get();
		DObject* Bound = Component.Get();
		if (Owner) Collector.AddReferencedObject(Owner);
		if (Bound) Collector.AddReferencedObject(Bound);
	}

	void FTransformGizmoTargetSet::AddComponent(DSceneComponent* Component)
	{
		if (!Component) throw std::invalid_argument("Missing transform component.");
		auto Binding = std::make_shared<FTransformComponentBinding>();
		Binding->Component = Component;
		auto* Actor = Component->GetOwner();
		if (Actor && Actor->GetRootComponent() == Component)
		{
			Binding->RootOwner = Actor;
			Binding->Options.Identity = {Actor};
			Binding->Options.Label = Actor->GetActorLabel();
		}
		else
		{
			Binding->Options.Identity = {Component};
			Binding->Options.Label = Component->GetName();
		}
		Targets.push_back(std::move(Binding));
	}

	void FTransformGizmoTargetSet::AddComponentCustom(DSceneComponent* Component, FComponentTransformGetter Getter,
		FComponentTransformSetter Setter, FTransformComponentOptions Options)
	{
		if (!Component || !Getter || !Setter) throw std::invalid_argument("Incomplete custom transform binding.");
		if (!Options.Identity.Object) Options.Identity.Object = Component;
		if (Options.Label.empty()) Options.Label = Component->GetName();
		Targets.push_back(std::make_shared<FTransformComponentBinding>(FTransformComponentBinding{
			TWeakObjectPtr<DSceneComponent>(Component), {}, std::move(Getter), std::move(Setter), std::move(Options)}));
	}
}
