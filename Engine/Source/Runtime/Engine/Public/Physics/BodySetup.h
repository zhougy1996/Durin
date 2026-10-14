#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Physics/BodySetupTypes.h"
#include "Physics/CookBodySetupInfo.h"
#include "DObject/Object.h"
#include "Physics/PhysicsTypes.h"

#include "BodySetup.gen.h"

namespace Durin
{
	// Identifies the authored simple geometry published by a body setup.
	DENUM()
	enum class EBodySetupShapeType : uint8
	{
		None,
		Box,
		Sphere,
		Capsule
	};

	// Owns reusable asset collision independently from render data and component transforms.
	DCLASS()
	class DBodySetup : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DBodySetup(const FObjectInitializer& ObjectInitializer);
		// Owner-thread lifecycle. Async capture owns its inputs and never retains the provider.
		// Every request notifies once: input/admission failures and disabled collision complete inline;
		// accepted work completes through the owner pump after task retirement.
		// Callbacks may reenter these APIs and must tolerate owner destruction.
		ENGINE_API auto InvalidatePhysicsData() -> void;
		ENGINE_API auto CreatePhysicsMeshesAsync(const IInterface_CollisionDataProvider& Provider,
			bool bPersistDerivedData = true, FOnAsyncPhysicsCookFinished Completion = {}) -> void;
		ENGINE_API auto CreatePhysicsMeshes(const IInterface_CollisionDataProvider& Provider,
			bool bPersistDerivedData = true) -> void;
		// Assemble the detached cook descriptor from prepared geometry and current body settings.
		ENGINE_API auto GetCookInfo(FTriMeshCollisionData Data, bool bPersistDerivedData = true) const -> FCookBodySetupInfo;
		ENGINE_API auto IsCompiling() const -> bool;
		ENGINE_API auto IsCompilationMutationBlocked() const -> bool;
		ENGINE_API auto AbortPhysicsMeshAsyncCreation() -> void;
		ENGINE_API auto FinishPhysicsMeshes() -> void;
		ENGINE_API auto BeginDestroy() -> void override;
		auto GetPhysicsMeshBuildStatus() const -> EPhysicsMeshBuildStatus
		{
			if (CollisionSourceMode == EBodySetupCollisionSourceMode::None || CachedSimpleCollision || CachedComplexCollision)
				return EPhysicsMeshBuildStatus::Ready;
			return PhysicsMeshStatus == EPhysicsMeshBuildStatus::Ready ? EPhysicsMeshBuildStatus::Unavailable : PhysicsMeshStatus;
		}
		auto GetPhysicsMeshBuildError() const -> const std::optional<std::string>& { return PhysicsMeshError; }
		auto GetPhysicsMeshRequestGeneration() const -> uint64 { return PhysicsMeshRequestGeneration; }
		// Stale requests return Superseded without changing the current state or its error.
		// A matching request that rejects geometry stores the installation failure once.
		ENGINE_API auto ApplyPhysicsMeshes(uint64 Generation, const FCollisionGeometryRef& Simple,
			const FCollisionGeometryRef& Complex) -> EPhysicsCookCompletionStatus;
		ENGINE_API auto FailPhysicsMeshes(uint64 Generation, std::string Error) -> void;
		ENGINE_API auto CancelPhysicsMeshes(uint64 Generation) -> void;
		ENGINE_API auto SetBox(const FVector3& HalfExtent, const FVector3& Center = FVector3(0.0)) -> bool;
		ENGINE_API auto SetSphere(double Radius, const FVector3& Center = FVector3(0.0)) -> bool;
		ENGINE_API auto SetCapsule(double Radius, double HalfHeight, const FVector3& Center = FVector3(0.0)) -> bool;
		ENGINE_API auto BuildShape(FCollisionShape& OutShape, FTransform& OutLocalTransform) const -> bool;
		ENGINE_API auto BuildGeometry(FCollisionGeometryRef& OutGeometry, FTransform& OutLocalTransform) const -> bool;
		ENGINE_API auto BuildSimpleGeometry(FCollisionGeometryRef& OutGeometry) const -> bool;
		ENGINE_API auto BuildComplexGeometry(FCollisionGeometryRef& OutGeometry) const -> bool;
		// Requires a declared EBodySetupCollisionSourceMode value.
		ENGINE_API auto SetCollisionSourceMode(EBodySetupCollisionSourceMode Mode) -> void;
		// Requires a declared EBodySetupCollisionQueryPolicy value.
		ENGINE_API auto SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy Policy) -> void;
		// Installs compatible immutable geometry and advances its revision.
		// Build/cache provenance is deliberately not retained by the physics owner.
		ENGINE_API auto SetCollisionGeometry(
			const FCollisionGeometryRef& Simple,
			const FCollisionGeometryRef& Complex) -> bool;
		ENGINE_API auto ClearCollisionGeometry() -> void;
		ENGINE_API auto IsValid(std::string* OutDiagnostic = nullptr) const -> bool;
		// Borrow installed geometry without constructing or refreshing primitive caches.
		auto GetResidentSimpleGeometry() const -> const FCollisionGeometryRef& { return CachedSimpleCollision; }
		auto GetResidentComplexGeometry() const -> const FCollisionGeometryRef& { return CachedComplexCollision; }
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetDimensions() const -> FVector3 { return Dimensions; }
		auto GetCenter() const -> FVector3 { return Center; }
		auto GetShapeType() const -> EBodySetupShapeType { return ShapeType; }
		auto GetCollisionSourceMode() const -> EBodySetupCollisionSourceMode { return CollisionSourceMode; }
		auto GetCollisionQueryPolicy() const -> EBodySetupCollisionQueryPolicy { return CollisionQueryPolicy; }
#if DURIN_WITH_EDITORONLY_DATA
		auto GetCollisionBuildRevision() const -> uint64 { return CollisionBuildRevision; }
#endif

	private:
		friend class DStaticMesh;
		bool bCompilationMutationBlocked = false;
		auto NotifyPhysicsDataChanged() -> void;
		EPhysicsMeshBuildStatus PhysicsMeshStatus = EPhysicsMeshBuildStatus::Ready;
		std::optional<std::string> PhysicsMeshError;
		uint64 PhysicsMeshRequestGeneration = 1;
		DPROPERTY()
		EBodySetupShapeType ShapeType = EBodySetupShapeType::None;

		// Box uses XYZ half extents; Sphere uses X radius; Capsule uses X radius and Z half height.
		DPROPERTY(Edit, Category = "Shape", ToolTip = "Shape dimensions in local space", Units = "Meters")
		FVector3 Dimensions{0.0};

		DPROPERTY(Edit)
		FVector3 Center{0.0};

		DPROPERTY()
		uint64 Revision = 1;

		DPROPERTY(Edit)
		EBodySetupCollisionSourceMode CollisionSourceMode = EBodySetupCollisionSourceMode::None;

		DPROPERTY(Edit)
		EBodySetupCollisionQueryPolicy CollisionQueryPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;

#if DURIN_WITH_EDITORONLY_DATA
		DPROPERTY(EditorOnly)
		uint64 CollisionBuildRevision = 0;
#endif

		mutable FCollisionGeometryRef CachedGeometry;
		mutable uint64 CachedGeometryRevision = 0;
		FCollisionGeometryRef CachedSimpleCollision;
		FCollisionGeometryRef CachedComplexCollision;
	};
}
