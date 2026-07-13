#pragma once

#include "Components/ActorComponent.h"

#include "SceneComponent.gen.h"

namespace Durin
{
	class DLevel;
	enum class EAttachmentTransformRule : uint8
	{
		KeepWorld,
		KeepRelative,
		SnapToTarget
	};

	enum class EDetachmentTransformRule : uint8
	{
		KeepWorld,
		KeepRelative
	};

	DCLASS()
	class DSceneComponent : public DActorComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto OnComponentPendingKill() -> void override;

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

private:
		DPROPERTY(Edit)
		FTransform RelativeTransform;

		DPROPERTY(Edit, ReadOnly, Transient)
		FTransform ComponentToWorld;

		auto CanAttachTo(const DSceneComponent* Parent) const -> bool;
		auto RemoveAttachChild(DSceneComponent* Child) -> void;

		DPROPERTY()
		TObjectPtr<DSceneComponent> AttachParent;

		DPROPERTY(Transient)
		std::vector<TObjectPtr<DSceneComponent>> AttachChildren;

		friend class DLevel;
	};
}
