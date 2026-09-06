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
		Result.BuilderVersion = StaticMeshCollisionBuilderVersion;
		Result.SchemaVersion = StaticMeshCollisionPayloadSchemaVersion;
		if (!BodySetup) return Result;
		Result.Mode = BodySetup->GetCollisionSourceMode();
		Result.Policy = BodySetup->GetCollisionQueryPolicy();
		Result.BuildRevision = BodySetup->GetCollisionBuildRevision();
		if (RenderData && !RenderData->LODResources.empty())
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
			&& Result.GeometryKind == ECollisionGeometryKind::TriangleMesh;
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

	namespace
	{
		auto InspectField(std::string Name, const FAssetPackageField* Field) -> FStaticMeshPayloadFieldInspection
		{
			FStaticMeshPayloadFieldInspection Result{.Field = std::move(Name)};
			if (!Field) { Result.Diagnostic = "Field absent; restore/reimport authored data or recook cooked data as appropriate."; return Result; }
			if (Field->SourceFormatVersion != ObjectPackage::DastV9FormatVersion)
			{ Result.State = "Unsupported"; Result.Diagnostic = "Descriptor format unsupported; readability unavailable."; return Result; }
			FEditorBulkDataStorageDescriptor Descriptor;
			if (!Field->TryReadBulkDataStorageDescriptor(Descriptor, false))
			{ Result.State = "Malformed"; Result.Diagnostic = "Malformed field descriptor; restore/reimport source or recook this field."; return Result; }
			Result.State = "Metadata present";
			Result.Placement = Descriptor.StorageKind == EEditorBulkDataStorageKind::External ? "Package companion range" : "Package inline";
			Result.LogicalBytes = Descriptor.LogicalByteCount;
			Result.StoredBytes = Descriptor.StoredByteCount;
			Result.Identity = Descriptor.ContentHash;
			Result.Diagnostic = "Readability unavailable: payload and companion were not read or validated.";
			return Result;
		}
	}

	auto InspectStaticMeshPayloadPackage(const FAssetPackageInspection& Package,
		FStaticMeshPayloadInspection& OutInspection, std::string* OutError) -> bool
	{
		OutInspection = {.bConstructFree = true, .Package = Package.PhysicalPath};
		if (Package.Header.AssetClassName != DStaticMesh::StaticClass()->GetQualifiedName().ToString())
		{
			if (OutError) *OutError = "Unsupported asset class for StaticMesh payload inspection.";
			return false;
		}
		const auto* Imported = Package.FindField("ImportedData");
		std::vector<FAssetPackageField> Fields;
		const FAssetPackageField* Geometry = nullptr;
		const bool bStructValid = Imported && Imported->TryInspectStructFields(Fields);
		if (bStructValid)
			for (const auto& Field : Fields) if (Field.Name == "Geometry") Geometry = &Field;
		auto Source = InspectField("ImportedData.Geometry", Geometry);
		if (Imported && (!bStructValid || !Geometry))
		{ Source.State = "Malformed"; Source.Diagnostic = "ImportedData geometry metadata is malformed; restore or reimport authored data."; }
		if (bStructValid)
		{
			uint32 Schema = 0;
			for (const auto& Field : Fields) if (Field.Name == "SchemaVersion") Field.TryReadScalar(Schema);
			if (Schema != StaticMeshImportedDataSchemaVersion)
			{ Source.State = "Unsupported"; Source.Diagnostic = "Authored source schema unavailable or unsupported; restore or reimport."; }
		}
		OutInspection.Fields.push_back(std::move(Source));
		OutInspection.Fields.push_back(InspectField("RenderData", Package.FindField("RenderData")));
		OutInspection.Fields.push_back(InspectField("CollisionData", Package.FindField("CollisionData")));
		if (OutError) OutError->clear();
		return true;
	}

	auto InspectStaticMeshPayloads(const DStaticMesh& Mesh) -> FStaticMeshPayloadInspection
	{
		FStaticMeshPayloadInspection Result;
		Result.Package = Mesh.GetObjectPath();
		const auto& Source = Mesh.GetImportedData();
		const auto& Bulk = Source.GetGeometryBulk();
		Result.Fields.push_back({.Field = "ImportedData.Geometry",
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
		Result.CookedLoad = Mesh.GetRenderDataLoadStatus();
		Result.Gpu = Mesh.GetRenderResourceStatus();
		Result.Operation = GetStaticMeshCompilationDiagnostic(Mesh);
		Result.Operation.Message.resize(std::min<size_t>(Result.Operation.Message.size(), 4096));
		Result.bOperationSourceMatches = Result.Operation.RequestId != 0
			&& Result.Operation.SourceIdentity == Source.GetIdentity();
		Result.Collision = InspectStaticMeshCollision(Mesh);
		return Result;
	}
}
