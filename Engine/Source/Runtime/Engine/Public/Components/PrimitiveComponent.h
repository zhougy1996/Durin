#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Rendering/PrimitiveSceneProxy.h"
#include "Physics/BodyInstance.h"
#include "Rendering/PrimitiveComponentId.h"
#include "PSOPrecacheComponent.h"

#include "PrimitiveComponent.gen.h"

namespace Durin
{
	class DWorld;
	class IRendererModule;
	enum class EPSOPrecacheProxyCreationStrategy : uint8 { AlwaysCreate, DelayUntilPSOPrecached };
	// Selects when a registered primitive may create and publish its physics body.
	enum class EPhysicsStateCreationPolicy : uint8
	{
		Eager,
		OnDemand,
		DeferredRequired
	};
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
		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API virtual auto PrecachePSOs() -> void;
		ENGINE_API virtual auto CollectPSOPrecacheData(const FPSOPrecacheParams& Params, FComponentPSOPrecacheParamsList& OutParams) -> void;
		ENGINE_API auto CheckPSOPrecachingAndBoostPriority(EPSOPrecachePriority Priority = EPSOPrecachePriority::Highest) -> bool;
		ENGINE_API auto IsPSOPrecacheReadyForProxyCreation() -> bool;
		ENGINE_API auto SetPSOPrecacheProxyCreationStrategy(EPSOPrecacheProxyCreationStrategy Strategy) -> void;
		auto GetPSOPrecacheProxyCreationStrategy() const -> EPSOPrecacheProxyCreationStrategy { return PSOPrecacheProxyCreationStrategy; }
		auto GetPSOPrecacheDiagnostic() const -> const std::string& { return PSOPrecacheDiagnostic; }
		// Called by engine/world owner ticks, including editor and paused worlds.
		ENGINE_API static auto ProcessPSOPrecacheRequests(DWorld* World = nullptr) -> void;
		ENGINE_API static auto InvalidatePSOPrecacheRequests() -> void;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto OnOwnerVisibilityChanged() -> void override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

		ENGINE_API virtual auto CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>;
		ENGINE_API auto GetRenderMatrix() const -> FMatrix;
		ENGINE_API auto GetPrimitiveComponentId() const -> FPrimitiveComponentId { return PrimitiveComponentId; }
		ENGINE_API auto DestroyRenderState() -> void;
		ENGINE_API auto RecreateRenderState() -> void;
		ENGINE_API auto MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags DirtyFlags = EPrimitiveRenderStateDirtyFlags::Proxy) -> void;
		auto GetPSOPrecacheComponentData() -> FPSOPrecacheComponentData& { return PSOPrecacheComponentData; }
		auto GetPSOPrecacheComponentData() const -> const FPSOPrecacheComponentData& { return PSOPrecacheComponentData; }
		auto SetMaterialPSOPrecacheData(std::vector<FMaterialPSOPrecacheRequestID> Requests, std::function<void()> Finished = {}) -> bool
		{ return PSOPrecacheComponentData.SetMaterialPSOPrecacheData(std::move(Requests), std::move(Finished)); }
		auto IsPSOPrecaching() const -> bool { return bPSOPrecacheSubmissionPending || PSOPrecacheComponentData.IsPSOPrecaching(); }
		ENGINE_API auto SetVisible(bool bInVisible) -> void;
		ENGINE_API auto SetCustomPrimitiveData(uint32 First, std::span<const float> Values) -> bool;
		auto GetCustomPrimitiveData() const -> const FPrimitiveCustomData& { return CustomPrimitiveData; }
		auto IsVisible() const -> bool { return bVisible; }
		ENGINE_API auto SetCollisionProfileName(FName ProfileName) -> bool;
		ENGINE_API auto SetCollisionEnabled(ECollisionEnabled Enabled) -> void;
		ENGINE_API auto SetCollisionObjectType(ECollisionChannel Channel) -> void;
		ENGINE_API auto SetCollisionResponseToChannel(ECollisionChannel Channel, ECollisionResponse Response) -> void;
		auto GetCollisionProfileName() const -> FName { return BodyInstance.CollisionProfileName; }
		auto GetCollisionEnabled() const -> ECollisionEnabled { return BodyInstance.CollisionEnabled; }
		auto GetCollisionObjectType() const -> ECollisionChannel { return BodyInstance.ObjectChannel; }
		auto GetPhysicsActorHandle() const -> FPhysicsActorHandle { return BodyInstance.ActorHandle; }
		auto GetPhysicsBodyMotionType() const -> EPhysicsBodyMotionType { return BodyInstance.MotionType; }
		auto GetPublishedBodySetupRevision() const -> uint64 { return BodyInstance.PublishedBodySetupRevision; }
		auto GetPhysicsRegistrationGeneration() const -> uint64 { return PhysicsRegistrationGeneration; }
		// Migrates an existing scene body between spatial partitions without exposing index state.
		ENGINE_API auto SetPhysicsBodyMotionType(EPhysicsBodyMotionType MotionType) -> void;
		ENGINE_API virtual auto BuildCollisionShape(FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool;
		ENGINE_API auto BuildPhysicsBodyDesc(FPhysicsBodyDesc& Desc) const -> bool;
		ENGINE_API auto RecreatePhysicsState() -> void;
		// Requests policy-owned creation; a required request may block only at an explicit lifecycle barrier.
		ENGINE_API virtual auto RequestPhysicsStateCreation(bool bWaitUntilReady = false) -> bool;


	protected:
		ENGINE_API auto OnComponentPendingKill() -> void override;
		ENGINE_API virtual auto PopulateCollisionGeometry(FPhysicsBodyDesc& Desc) const -> bool;
		ENGINE_API auto OnUpdateTransform() -> void override;
		ENGINE_API virtual auto BuildMaterialRenderProxyBindingUpdate(
			FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool;
		ENGINE_API virtual auto GetCollisionStateRevision() const -> uint64 { return 0; }
		ENGINE_API virtual auto GetPhysicsStateCreationPolicy() const -> EPhysicsStateCreationPolicy;
		ENGINE_API virtual auto OnCollisionSettingsChanged() -> void;
		auto DestroyPhysicsState() -> void;
		auto GetPhysicsWorld() const -> DWorld*;

	private:
		auto GetPSOPrecacheRenderer() const -> IRendererModule*;
		auto CreateRenderState() -> void;
		ENGINE_API auto EnsurePrimitiveComponentId() -> FPrimitiveComponentId;
		auto UpdatePhysicsState() -> void;
		auto ApplyPhysicsStateCreationPolicy() -> void;

		FPrimitiveComponentId PrimitiveComponentId = InvalidPrimitiveComponentId;
		FPrimitiveCustomData CustomPrimitiveData;
		FPSOPrecacheComponentData PSOPrecacheComponentData;
		bool bPSOPrecacheSubmissionPending = false;
		bool bProcessingPSOPrecache = false;
		EPSOPrecacheProxyCreationStrategy PSOPrecacheProxyCreationStrategy = EPSOPrecacheProxyCreationStrategy::DelayUntilPSOPrecached;
		std::string PSOPrecacheDiagnostic;
		// Non-owning publication token; the scene keeps the proxy alive until its removal command runs.
		FPrimitiveSceneProxy* SceneProxy = nullptr;
		uint64 PhysicsRegistrationGeneration = 0;
		mutable FCollisionGeometryRef CachedCollisionGeometry;
		DPROPERTY(Category = "Rendering", Edit)
		bool bVisible = true;
		DPROPERTY(Category = "Collision", Edit)
		FBodyInstance BodyInstance;

		friend class FScene;
	};
}
