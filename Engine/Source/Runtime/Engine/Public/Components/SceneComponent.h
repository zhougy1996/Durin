#pragma once

#include "Components/ActorComponent.h"
#include "DObject/ObjectPtr.h"

#include "SceneComponent.gen.h"

namespace Durin
{
	class DLevel;
	class FSceneInterface;

	// Selects which transform is preserved or snapped while attaching a component.
	enum class EAttachmentTransformRule : uint8
	{
		KeepWorld,
		KeepRelative,
		SnapToTarget
	};

	// Selects whether detachment preserves world or relative transform values.
	enum class EDetachmentTransformRule : uint8
	{
		KeepWorld,
		KeepRelative
	};

	// Maintains relative/world transforms and an acyclic attachment hierarchy.
	DCLASS()
	class DSceneComponent : public DActorComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto OnComponentPendingKill() -> void override;
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

		ENGINE_API auto GetRelativeTransform() const -> const FTransform&;
		ENGINE_API auto SetRelativeTransform(const FTransform& InTransform) -> void;

		ENGINE_API auto GetWorldTransform() const -> const FTransform&;
		ENGINE_API auto SetWorldTransform(const FTransform& InTransform) -> void;

		ENGINE_API auto GetRelativeLocation() const -> const FVector3&;
		ENGINE_API auto SetRelativeLocation(const FVector3& InLocation) -> void;

		ENGINE_API auto GetRelativeRotation() const -> const FQuat&;
		ENGINE_API auto SetRelativeRotation(const FQuat& InRotation) -> void;

		ENGINE_API auto GetRelativeScale3D() const -> const FVector3&;
		ENGINE_API auto SetRelativeScale3D(const FVector3& InScale) -> void;

		ENGINE_API auto GetWorldLocation() const -> const FVector3&;
		ENGINE_API auto SetWorldLocation(const FVector3& InLocation) -> void;

		ENGINE_API auto GetWorldRotation() const -> const FQuat&;
		ENGINE_API auto SetWorldRotation(const FQuat& InRotation) -> void;

		ENGINE_API auto GetWorldScale3D() const -> const FVector3&;
		ENGINE_API auto SetWorldScale3D(const FVector3& InScale) -> void;

		ENGINE_API auto SetupAttachment(DSceneComponent* Parent) -> bool;
		ENGINE_API auto AttachToComponent(DSceneComponent* Parent, EAttachmentTransformRule Rule = EAttachmentTransformRule::KeepWorld) -> bool;
		ENGINE_API auto DetachFromComponent(EDetachmentTransformRule Rule = EDetachmentTransformRule::KeepWorld) -> bool;

		auto GetAttachParent() const -> DSceneComponent* { return AttachParent.Get(); }
		auto GetAttachChildren() const -> const std::vector<TObjectPtr<DSceneComponent>>& { return AttachChildren; }
		ENGINE_API auto IsAttachedTo(const DSceneComponent* Component) const -> bool;

		ENGINE_API auto UpdateComponentToWorld() -> void;
		ENGINE_API auto GetComponentToWorldMatrix() const -> FMatrix;

protected:
		ENGINE_API virtual auto OnUpdateTransform() -> void;
		auto GetRenderScene() const -> FSceneInterface* { return RenderScene; }

private:
		// Authored transform relative to AttachParent, or world-relative when unattached.
		DPROPERTY(Edit)
		FTransform RelativeTransform;

		// Derived world transform rebuilt from RelativeTransform and the attachment chain.
		DPROPERTY(Edit, ReadOnly, Transient, MetaData="DefaultCollapsed")
		FTransform ComponentToWorld;

		auto CanAttachTo(const DSceneComponent* Parent) const -> bool;
		auto RemoveAttachChild(DSceneComponent* Child) -> void;

		DPROPERTY()
		TObjectPtr<DSceneComponent> AttachParent;

		DPROPERTY(Transient)
		std::vector<TObjectPtr<DSceneComponent>> AttachChildren;

		FSceneInterface* RenderScene = nullptr;

		friend class DLevel;
	};
}
