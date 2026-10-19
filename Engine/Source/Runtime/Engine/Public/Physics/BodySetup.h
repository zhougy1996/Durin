#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Physics/BodySetupTypes.h"
#include "Physics/CookBodySetupInfo.h"
#include "DObject/Object.h"
#include "DObject/ObjectKey.h"
#include "DObject/StructOps.h"
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
		Capsule,
		Convex
	};

	DENUM()
	enum class EBodySetupCollisionTraceMode : uint8 { SimpleAndComplex, SimpleAsComplex, ComplexAsSimple };

	// Runtime mesh-local units, independent of imported source coordinates and collision LOD.
	DSTRUCT()
	struct FSimpleCollisionElement
	{
		GENERATED_BODY()
		DPROPERTY()
		EBodySetupShapeType Type = EBodySetupShapeType::None;
		DPROPERTY()
		FVector3 Dimensions{0.0};
		DPROPERTY()
		FTransform Transform;
		DPROPERTY()
		std::vector<FVector3> Vertices;
		// Validated hull topology is retained in authored and cooked packages; empty means cook from points.
		DPROPERTY()
		std::vector<uint32> Indices;
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
		// Withdraw unstarted cooks and invalidate this request's publication. Started
		// cooks finish normally; FinishPhysicsMeshes waits for their task/callback retirement.
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
		// Atomically replaces the flat authored aggregate (up to 64 elements). Empty restores the legacy shape.
		ENGINE_API auto SetSimpleCollision(std::vector<FSimpleCollisionElement> Elements) -> bool;
		auto GetSimpleCollision() const -> const std::vector<FSimpleCollisionElement>& { return SimpleCollision; }
		ENGINE_API auto SetCollisionTraceMode(EBodySetupCollisionTraceMode Mode) -> void;
		auto GetCollisionTraceMode() const -> EBodySetupCollisionTraceMode { return CollisionTraceMode; }
		// Publishes both representations using component-world coordinates; Geometry remains the legacy default.
		ENGINE_API auto ConfigureQueryGeometry(FPhysicsBodyDesc& Desc, const FTransform& WorldTransform) const -> void;
		ENGINE_API auto BuildShape(FCollisionShape& OutShape, FTransform& OutLocalTransform) const -> bool;
		ENGINE_API auto BuildGeometry(FCollisionGeometryRef& OutGeometry, FTransform& OutLocalTransform) const -> bool;
		ENGINE_API auto BuildSimpleGeometry(FCollisionGeometryRef& OutGeometry) const -> bool;
		ENGINE_API auto BuildComplexGeometry(FCollisionGeometryRef& OutGeometry) const -> bool;
		// Requires a declared EBodySetupCollisionSourceMode value.
		ENGINE_API auto SetCollisionSourceMode(EBodySetupCollisionSourceMode Mode) -> void;
		// Requires a declared EBodySetupCollisionQueryPolicy value.
		ENGINE_API auto SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy Policy) -> void;
		// Installs external immutable geometry, supersedes prior requests and advances publication.
		// Build/cache provenance is deliberately not retained by the physics owner.
		ENGINE_API auto InstallPhysicsMeshes(
			const FCollisionGeometryRef& Simple,
			const FCollisionGeometryRef& Complex) -> bool;
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
		friend class FScopedBodySetupUpdate;
		auto BeginUpdate() -> void;
		auto EndUpdate() -> void;
		auto MarkChanged(EBodySetupChange Changes) -> void;
		auto InvalidateSimpleGeometry() -> void;
		auto FlushChanges() -> void;
		auto PublishPhysicsMeshes(const FCollisionGeometryRef& Simple, const FCollisionGeometryRef& Complex, bool bSupersedeRequest) -> bool;
		uint32 UpdateDepth = 0;
		EBodySetupChange PendingChanges = EBodySetupChange::None;
		uint64 SimpleGeometryRevision = 1;
		bool bPhysicsDataInvalidated = false;
		bool bCompilationMutationBlocked = false;
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

		DPROPERTY()
		std::vector<FSimpleCollisionElement> SimpleCollision;
		DPROPERTY()
		EBodySetupCollisionTraceMode CollisionTraceMode = EBodySetupCollisionTraceMode::SimpleAndComplex;
		mutable FCollisionGeometryRef CachedAggregate;
		mutable uint64 CachedAggregateRevision = 0;
		mutable FCollisionGeometryRef CachedGeometry;
		mutable uint64 CachedGeometryRevision = 0;
		FCollisionGeometryRef CachedSimpleCollision;
		FCollisionGeometryRef CachedComplexCollision;
	};
	// Owner-thread batching: inputs and caches change immediately; revision and owner notification
	// publish once at the outer scope exit. Synchronous cooks may finish inside the scope.
	class FScopedBodySetupUpdate final
	{
	public:
		ENGINE_API explicit FScopedBodySetupUpdate(DBodySetup& Body);
		ENGINE_API ~FScopedBodySetupUpdate();
		FScopedBodySetupUpdate(const FScopedBodySetupUpdate&) = delete;
		auto operator=(const FScopedBodySetupUpdate&) -> FScopedBodySetupUpdate& = delete;
	private:
		FObjectKey Body;
	};

}
