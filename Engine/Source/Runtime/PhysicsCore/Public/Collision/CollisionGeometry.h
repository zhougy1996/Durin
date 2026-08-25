#pragma once

#include "Collision/CollisionShape.h"
#include "Math/Transform.h"

namespace Durin
{
	struct FPhysicsQueryHit;

	// Describes one stable simple child supplied to immutable compound geometry creation.
	struct FCollisionGeometryChild
	{
		FCollisionShape Shape;
		FTransform LocalTransform;
	};

	// Identifies the immutable payload behind a geometry reference without exposing storage.
	enum class ECollisionGeometryKind : uint8
	{
		Primitive,
		Compound,
		ConvexHull,
		TriangleMesh,
		HeightField
	};

	// Stable indexed triangle retained by hull and mesh resources.
	struct FCollisionGeometryTriangle
	{
		uint32 First = 0;
		uint32 Second = 0;
		uint32 Third = 0;
		uint32 SourceOrdinal = 0;
	};

	// Frozen 32-byte deterministic binary BVH node. Leaf counts carry the high-bit marker.
	struct FCollisionGeometryNode
	{
		FVector3f Minimum{0.0f};
		uint32 First = 0;
		FVector3f Maximum{0.0f};
		uint32 CountOrSecond = 0;

		auto IsLeaf() const -> bool { return (CountOrSecond & 0x80000000u) != 0; }
		auto GetLeafCount() const -> uint32 { return CountOrSecond & 0x7fffffffu; }
	};

	// Identifies one rectangular HeightField leaf in cell coordinates.
	struct FCollisionHeightFieldRegion
	{
		uint32 OriginX = 0;
		uint32 OriginY = 0;
		uint32 CellCountX = 0;
		uint32 CellCountY = 0;
	};

	struct FCollisionHullPlane
	{
		FVector3f Normal{0.0f};
		float Distance = 0.0f;
	};

	struct FCollisionHullHalfEdge
	{
		uint32 Origin = 0;
		uint32 Twin = 0;
		uint32 Next = 0;
		uint32 Face = 0;
	};

	struct FCollisionHullFace
	{
		uint32 FirstEdge = 0;
		uint32 EdgeCount = 0;
		uint32 SourceOrdinal = 0;
		uint32 Reserved = 0;
	};

	enum class ECollisionGeometryBuildStatus : uint8
	{
		Success,
		InvalidInput,
		EmptyAfterCleanup,
		LimitExceeded,
		DepthExceeded,
		AllocationFailed
	};

	struct FCollisionGeometryBuildDiagnostics
	{
		ECollisionGeometryBuildStatus Status = ECollisionGeometryBuildStatus::InvalidInput;
		uint32 SourceVertices = 0;
		uint32 RetainedVertices = 0;
		uint32 SourceTriangles = 0;
		uint32 RetainedTriangles = 0;
		uint32 RemovedTriangles = 0;
		uint32 NodeCount = 0;
		uint32 MaximumDepth = 0;
		uint64 RetainedBytes = 0;
		uint64 EstimatedPeakBytes = 0;
		uint64 HashNanoseconds = 0;
		uint64 MatchNanoseconds = 0;
		uint64 SampleCopyNanoseconds = 0;
		uint64 TreeBuildNanoseconds = 0;
		bool bCacheHit = false;
	};

	class FCollisionGeometry;

	// Copyable owning reference to one validated immutable collision payload.
	class FCollisionGeometryRef
	{
	public:
		FCollisionGeometryRef() = default;
		PHYSICSCORE_API static auto MakePrimitive(const FCollisionShape& Shape) -> FCollisionGeometryRef;
		PHYSICSCORE_API static auto MakeCompound(std::span<const FCollisionGeometryChild> Children) -> FCollisionGeometryRef;
		PHYSICSCORE_API static auto MakeConvexHull(
			std::span<const FVector3> Vertices,
			std::span<const uint32> Indices) -> FCollisionGeometryRef;
		PHYSICSCORE_API static auto MakeTriangleMesh(
			std::span<const FVector3> Vertices,
			std::span<const uint32> Indices,
			std::span<const uint32> SourceOrdinals = {}) -> FCollisionGeometryRef;
		PHYSICSCORE_API static auto MakeCookedTriangleMesh(
			std::span<const FVector3> Vertices,
			std::span<const uint32> Indices,
			std::span<const uint32> SourceOrdinals,
			std::span<const FCollisionGeometryNode> Nodes,
			std::span<const uint32> LeafTriangles) -> FCollisionGeometryRef;
		PHYSICSCORE_API static auto BuildConvexHull(
			std::span<const FVector3> Points,
			FCollisionGeometryBuildDiagnostics* Diagnostics = nullptr) -> FCollisionGeometryRef;
		PHYSICSCORE_API static auto BuildTriangleMesh(
			std::span<const FVector3> Vertices,
			std::span<const uint32> Indices,
			FCollisionGeometryBuildDiagnostics* Diagnostics = nullptr) -> FCollisionGeometryRef;
		// Copies one top-left row-major sample plane into a bounded regular-grid query surface.
		PHYSICSCORE_API static auto BuildHeightField(
			uint32 Width,
			uint32 Height,
			std::span<const uint16> Samples,
			double SpacingX,
			double SpacingY,
			double HeightScale,
			double HeightOffset,
			FCollisionGeometryBuildDiagnostics* Diagnostics = nullptr) -> FCollisionGeometryRef;

		auto IsValid() const -> bool { return Payload != nullptr; }
		explicit operator bool() const { return IsValid(); }
		PHYSICSCORE_API auto GetKind() const -> ECollisionGeometryKind;
		PHYSICSCORE_API auto GetIdentity() const -> uint64;
		PHYSICSCORE_API auto GetChildCount() const -> uint32;
		PHYSICSCORE_API auto GetChild(uint32 Index) const -> const FCollisionGeometryChild*;
		PHYSICSCORE_API auto GetVertexCount() const -> uint32;
		PHYSICSCORE_API auto GetVertex(uint32 Index) const -> const FVector3*;
		PHYSICSCORE_API auto GetTriangleCount() const -> uint32;
		PHYSICSCORE_API auto GetTriangle(uint32 Index) const -> const FCollisionGeometryTriangle*;
		PHYSICSCORE_API auto GetTriangleVertices(
			uint32 Index, FVector3& OutFirst, FVector3& OutSecond, FVector3& OutThird,
			uint32* OutSourceOrdinal = nullptr) const -> bool;
		PHYSICSCORE_API auto GetNodeCount() const -> uint32;
		PHYSICSCORE_API auto GetNode(uint32 Index) const -> const FCollisionGeometryNode*;
		PHYSICSCORE_API auto GetLeafTriangleCount() const -> uint32;
		PHYSICSCORE_API auto GetLeafTriangle(uint32 Index) const -> uint32;
		PHYSICSCORE_API auto GetHeightFieldWidth() const -> uint32;
		PHYSICSCORE_API auto GetHeightFieldHeight() const -> uint32;
		PHYSICSCORE_API auto GetHeightFieldSample(uint32 X, uint32 Y, uint16& OutSample) const -> bool;
		PHYSICSCORE_API auto GetHeightFieldSpacing(double& OutX, double& OutY) const -> bool;
		PHYSICSCORE_API auto GetHeightFieldHeightRange(double& OutScale, double& OutOffset) const -> bool;
		PHYSICSCORE_API auto GetHeightFieldRegionCount() const -> uint32;
		PHYSICSCORE_API auto GetHeightFieldRegion(uint32 Index) const -> const FCollisionHeightFieldRegion*;
		PHYSICSCORE_API auto GetHullPlaneCount() const -> uint32;
		PHYSICSCORE_API auto GetHullPlane(uint32 Index) const -> const FCollisionHullPlane*;
		PHYSICSCORE_API auto GetHullHalfEdgeCount() const -> uint32;
		PHYSICSCORE_API auto GetHullHalfEdge(uint32 Index) const -> const FCollisionHullHalfEdge*;
		PHYSICSCORE_API auto GetHullFaceCount() const -> uint32;
		PHYSICSCORE_API auto GetHullFace(uint32 Index) const -> const FCollisionHullFace*;
		PHYSICSCORE_API auto GetLocalBounds(FVector3& OutMin, FVector3& OutMax) const -> bool;
		PHYSICSCORE_API auto GetRetainedBytes() const -> uint64;

	private:
		explicit FCollisionGeometryRef(std::shared_ptr<const FCollisionGeometry> InPayload)
			: Payload(std::move(InPayload))
		{}

		std::shared_ptr<const FCollisionGeometry> Payload;
	};
}

namespace Durin::CollisionGeometry
{
	// Reports the complete internal outcome without changing scene bool query APIs.
	enum class ECollisionQueryStatus : uint8
	{
		Hit,
		Miss,
		Invalid,
		Unsupported,
		NonConverged
	};

	// Keeps the retained oracle distinct from bounded production selection.
	enum class ECollisionQueryAlgorithm : uint8
	{
		Reference,
		Production
	};

	// Optional zero-allocation sink for bounded reference-geometry work.
	struct FCollisionGeometryCounters
	{
		uint64 DistanceEvaluations = 0;
		uint64 SearchIterations = 0;
		uint64 LeafTests = 0;
		uint64 FeatureTests = 0;
		uint64 AssetNodeTests = 0;
		uint64 AssetLeafTests = 0;
		uint64 HeightFieldCellTests = 0;
		uint64 HeightFieldTriangleTests = 0;
		uint64 CompoundChildren = 0;
		uint64 AnalyticDispatches = 0;
		uint64 GenericDispatches = 0;
		uint64 SupportEvaluations = 0;
		uint64 NonConverged = 0;
		uint64 Unsupported = 0;
		uint64 ReferenceFallbacks = 0;
		bool bOverflowed = false;
	};

	PHYSICSCORE_API auto Raycast(
		const FVector3& Start,
		const FVector3& End,
		const FCollisionGeometryRef& Target,
		const FTransform& TargetTransform,
		ECollisionQueryAlgorithm Algorithm,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> ECollisionQueryStatus;

	PHYSICSCORE_API auto Sweep(
		const FCollisionShape& Query,
		const FTransform& QueryTransform,
		const FVector3& Delta,
		const FCollisionGeometryRef& Target,
		const FTransform& TargetTransform,
		ECollisionQueryAlgorithm Algorithm,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> ECollisionQueryStatus;

	PHYSICSCORE_API auto Overlap(
		const FCollisionShape& Query,
		const FTransform& QueryTransform,
		const FCollisionGeometryRef& Target,
		const FTransform& TargetTransform,
		ECollisionQueryAlgorithm Algorithm,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> ECollisionQueryStatus;

	// Traces a finite segment against a positive-scale oriented box.
	PHYSICSCORE_API auto RaycastBox(
		const FVector3& Start,
		const FVector3& End,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> bool;

	// Tests a capsule against a positive-scale oriented box and reports bounded penetration.
	PHYSICSCORE_API auto OverlapCapsuleBox(
		const FCollisionShape& Capsule,
		const FTransform& CapsuleTransform,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> bool;

	// Sweeps a capsule by Delta against a positive-scale oriented box.
	PHYSICSCORE_API auto SweepCapsuleBox(
		const FCollisionShape& Capsule,
		const FTransform& CapsuleTransform,
		const FVector3& Delta,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> bool;
}
