#include "Physics/PhysicsDerivedData.h"
#include "Collision/CollisionGeometry.h"
#include "Diagnostics/StaticMeshPayloadInspection.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	auto InspectStaticMeshCollision(const DStaticMesh& Mesh) -> FStaticMeshCollisionInspection
	{
		FStaticMeshCollisionInspection Result{};
		const auto* BodySetup = Mesh.GetBodySetup();
		const auto* RenderData = Mesh.GetRenderData();
		Result.Mode = EBodySetupCollisionSourceMode::None;
		Result.Policy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		Result.GeometryKind = ECollisionGeometryKind::Primitive;
		Result.BuilderVersion = FCollisionCookedData::GetBuildVersion();
		Result.SchemaVersion = PhysicsCollisionPayloadSchemaVersion;
		if (!BodySetup) return Result;
		Result.Mode = BodySetup->GetCollisionSourceMode();
		Result.Policy = BodySetup->GetCollisionQueryPolicy();
		Result.BuildRevision = BodySetup->GetCollisionBuildRevision();
		const auto ComplexTriangles = Mesh.GetComplexCollisionSourceTriangleCount();
		if (Result.Mode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)
			Result.SourceTriangles = ComplexTriangles.value_or(0);
		else if (RenderData && !RenderData->LODResources.empty())
			Result.SourceTriangles = static_cast<uint32>(
				RenderData->LODResources.front().IndexBuffer.GetIndices().size() / 3);
		FCollisionGeometryRef Geometry;
		Geometry = BodySetup->GetResidentSimpleGeometry();
		if (!Geometry) Geometry = BodySetup->GetResidentComplexGeometry();
		if (!Geometry)
		{
			// No installed derived geometry; do not create primitive geometry for inspection.
			return Result;
		}
		Result.GeometryKind = Geometry.GetKind();
		Result.bHasGeometry = true;
		Result.RetainedTriangles = Geometry.GetTriangleCount();
		Result.bTriangleCountsComparable = Result.Mode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0
			&& ComplexTriangles.has_value() && Result.GeometryKind == ECollisionGeometryKind::TriangleMesh;
		Result.RemovedTriangles = Result.bTriangleCountsComparable && Result.SourceTriangles >= Result.RetainedTriangles
			? Result.SourceTriangles - Result.RetainedTriangles : 0;
		Result.Nodes = Geometry.GetNodeCount();
		Result.RuntimeBytes = Geometry.GetRetainedBytes();
		FVector3 Minimum;
		FVector3 Maximum;
		if (Geometry.GetLocalBounds(Minimum, Maximum)) Result.Bounds = FBox(Minimum, Maximum);
		// Build revision is not a source identity. Coherence remains unavailable.
		return Result;
	}

	auto InspectStaticMeshPayloads(const DStaticMesh& Mesh) -> FStaticMeshPayloadInspection
	{
		FStaticMeshPayloadInspection Result;
		Result.Package = Mesh.GetObjectPath();
		const auto& Source = Mesh.GetSource();
		const auto& Bulk = Source.GetGeometryBulk();
		Result.Fields.push_back({.Field = "Source.Geometry",
			.State = Source.IsValid() ? "Metadata present" : "Absent or invalid",
			.Placement = Bulk.IsMemoryResident() ? "Canonical memory" : "Package resource or absent",
			.LogicalBytes = Bulk.GetPayloadSize(), .StoredBytes = Bulk.GetPayloadSize(),
			.Identity = Source.GetIdentity(),
			.Diagnostic = "Storage readability unverified; restore/reimport authored data if acquisition fails."});
		Result.bSourceResident = Source.IsGeometryResident();
		for (const auto& [Name, Data] : {std::pair{"RenderData", &Mesh.GetCookedRenderData()},
			std::pair{"CollisionData", &Mesh.GetCookedCollisionData()}})
		{
			const auto Metadata = Data->GetMetadata();
			Result.Fields.push_back({.Field = Name,
				.State = Metadata.LogicalSize ? "Metadata present" : "Absent",
				.Placement = Data->GetState() == EBulkDataState::Attached ? "Package resource range" : "Runtime bulk",
				.LogicalBytes = Metadata.LogicalSize, .StoredBytes = Metadata.Range.StoredSize,
				.Diagnostic = "Cooked storage readability unverified; recook missing or invalid cooked data."});
		}
		Result.bCpuResident = Mesh.GetRenderData() != nullptr;
		Result.CookedLoadError = Mesh.GetRenderDataLoadError();
		Result.bCookedLoadPending = Mesh.HasPendingRenderDataLoad();
		Result.Gpu = Mesh.GetRenderResourceStatus();
		Result.Operation = Mesh.GetCompilationDiagnostic();
		Result.bOperationSourceMatches = Result.Operation.RequestId != 0
			&& Result.Operation.SourceIdentity == Source.GetIdentity();
		Result.Collision = InspectStaticMeshCollision(Mesh);
		return Result;
	}
}
