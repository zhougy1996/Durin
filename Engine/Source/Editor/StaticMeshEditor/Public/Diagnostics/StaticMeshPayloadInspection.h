#pragma once

#include "Asset/PackageInspection.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "StaticMeshEditorAPI.h"

namespace Durin
{
	// Bounded value-only collision facts for diagnostics and the read-only Inspector.
	struct FStaticMeshCollisionInspection
	{
		EBodySetupCollisionSourceMode Mode{};
		EBodySetupCollisionQueryPolicy Policy{};
		ECollisionGeometryKind GeometryKind{};
		bool bHasGeometry = false;
		uint32 SourceTriangles = 0;
		uint32 RetainedTriangles = 0;
		uint32 RemovedTriangles = 0;
		bool bTriangleCountsComparable = false;
		uint32 Nodes = 0;
		std::optional<FBox> Bounds;
		uint64 RuntimeBytes = 0;
		uint32 BuilderVersion = 0;
		uint32 SchemaVersion = 0;
		uint64 BuildRevision = 0;
		// No stored source correspondence exists; a revision alone cannot prove coherence.
		bool bRevisionCoherent = false;
	};

	// Describes metadata presence separately from physical readability, which is never probed.
	struct FStaticMeshPayloadFieldInspection
	{
		std::string Field;
		std::string State = "Absent";
		std::string Placement = "Unavailable";
		uint64 LogicalBytes = 0;
		uint64 StoredBytes = 0;
		FXxHash128 Identity;
		std::string Diagnostic;
	};

	// Owner-thread value snapshot. Operation history is explicitly separate from live readiness.
	struct FStaticMeshPayloadInspection
	{
		bool bConstructFree = false;
		std::string Package;
		std::vector<FStaticMeshPayloadFieldInspection> Fields;
		bool bSourceResident = false;
		bool bCpuResident = false;
		FCookedMeshLoadStatus CookedLoad;
		FStaticMeshRenderResourceStatus Gpu;
		FStaticMeshCompilationDiagnostic Operation;
		bool bOperationSourceMatches = false;
		FStaticMeshCollisionInspection Collision;
	};

	// Parses supplied package metadata only; never constructs objects, hashes payloads or opens companions.
	STATICMESHEDITOR_API auto InspectStaticMeshPayloadPackage(const FAssetPackageInspection& Package,
		FStaticMeshPayloadInspection& OutInspection, std::string* OutError = nullptr) -> bool;
	// Owner-thread queries never load, acquire source, call a provider/cache, initialize, dirty or repair.
	STATICMESHEDITOR_API auto InspectStaticMeshPayloads(const DStaticMesh& Mesh) -> FStaticMeshPayloadInspection;
	STATICMESHEDITOR_API auto InspectStaticMeshCollision(const DStaticMesh& Mesh) -> FStaticMeshCollisionInspection;
}
