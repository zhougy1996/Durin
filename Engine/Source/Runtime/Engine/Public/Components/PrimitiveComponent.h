#pragma once
#include "Components/SceneComponent.h"
#include "Engine/FPrimitiveSceneProxy.h"
#include "IScene.h"
#include "Physics/BodyInstance.h"

#include "PrimitiveComponent.gen.h"

namespace Durin
{
	class DWorld;
	enum class EEditorPickingPrimitiveFamily : uint8;
	// Selects the renderer-side state that must be synchronized for a primitive.
	enum class EPrimitiveRenderStateDirtyFlags : uint8
	{
		None = 0,
		Proxy = 1 << 0,
		Transform = 1 << 1,
		MaterialBinding = 1 << 2,
		Visibility = 1 << 3
	};
	ENUM_CLASS_FLAGS(EPrimitiveRenderStateDirtyFlags);

	// Owns a stable scene primitive identity and synchronizes proxy, transform, and material changes.
	DCLASS()
	class DPrimitiveComponent : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DPrimitiveComponent(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto OnOwnerVisibilityChanged() -> void override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

		ENGINE_API virtual auto CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>;
		ENGINE_API auto GetRenderMatrix() const -> FMatrix;
		ENGINE_API auto GetPrimitiveSceneId() const -> FPrimitiveSceneId { return PrimitiveSceneId; }
		ENGINE_API auto DestroyRenderState() -> void;
		ENGINE_API auto RecreateRenderState() -> void;
		ENGINE_API auto MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags DirtyFlags = EPrimitiveRenderStateDirtyFlags::Proxy) -> void;
		ENGINE_API auto SetCollisionProfileName(FName ProfileName) -> bool;
		ENGINE_API auto SetCollisionEnabled(ECollisionEnabled Enabled) -> void;
		ENGINE_API auto SetCollisionObjectType(ECollisionChannel Channel) -> void;
		ENGINE_API auto SetCollisionResponseToChannel(ECollisionChannel Channel, ECollisionResponse Response) -> void;
		auto GetCollisionProfileName() const -> FName { return BodyInstance.ProfileName; }
		auto GetCollisionEnabled() const -> ECollisionEnabled { return BodyInstance.CollisionEnabled; }
		auto GetCollisionObjectType() const -> ECollisionChannel { return BodyInstance.ObjectChannel; }
		auto GetPhysicsActorHandle() const -> FPhysicsActorHandle { return BodyInstance.ActorHandle; }
		auto GetPhysicsBodyMotionType() const -> EPhysicsBodyMotionType { return BodyInstance.MotionType; }
		// Migrates an existing scene body between spatial partitions without exposing index state.
		ENGINE_API auto SetPhysicsBodyMotionType(EPhysicsBodyMotionType MotionType) -> void;
		ENGINE_API virtual auto BuildCollisionShape(FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool;
		ENGINE_API virtual auto BuildCollisionGeometry(
			FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool;
		ENGINE_API auto RecreatePhysicsState() -> void;

#if DURIN_WITH_EDITOR
		// Produces finite local bounds and a supported picking family for the editor scene index.
		ENGINE_API virtual auto GetEditorPickingLocalBounds(FBox& OutBounds, EEditorPickingPrimitiveFamily& OutFamily) const -> bool;
#endif

	protected:
		ENGINE_API auto OnUpdateTransform() -> void override;
		ENGINE_API virtual auto BuildMaterialRenderProxyBindingUpdate(
			FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool;
#if DURIN_WITH_EDITOR
		auto NotifyEditorPickingMutation(bool bRetired = false) -> void;
#endif

	private:
		auto EnsurePrimitiveSceneId() -> FPrimitiveSceneId;
		auto DestroyPhysicsState() -> void;
		auto UpdatePhysicsState() -> void;
		auto GetPhysicsWorld() const -> DWorld*;

		FPrimitiveSceneId PrimitiveSceneId = InvalidPrimitiveSceneId;
		mutable FCollisionGeometryRef CachedCollisionGeometry;
		DPROPERTY(Edit)
		FBodyInstance BodyInstance;
	};
}
