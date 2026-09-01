#pragma once

#include "StaticMeshBuildAPI.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	// Immutable GameThread capture consumed by pure StaticMesh recipe work.
	struct FStaticMeshReconciliationSnapshot
	{
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		float NormalizedSize = 1.5f;
		std::string StableObjectPath;
	};

	class STATICMESHBUILD_API FStaticMeshBuildOperations
	{
	public:
		// GameThread adapter: captures the only mutable asset facts recipe work may observe.
		static auto CaptureReconciliationSnapshot(const DStaticMesh& Mesh)
			-> FStaticMeshReconciliationSnapshot;

		static auto BuildAndPublishImported(
			DStaticMesh& Mesh,
			const FStaticMeshImportedData& ImportedData,
			std::string_view SourceLabel,
			std::string& OutError) -> bool;

		static auto BuildImportedProduct(
			const FStaticMeshReconciliationSnapshot& Reconciliation,
			const FStaticMeshImportedData& ImportedData,
			std::string_view SourceLabel,
			FStaticMeshBuildProduct& OutProduct,
			std::string& OutError) -> bool;

		// Queries a validated cached product using metadata-only authored identity.
		static auto TryLoadImportedProduct(
			const FStaticMeshReconciliationSnapshot& Reconciliation,
			const FStaticMeshImportedData& ImportedData,
			FStaticMeshBuildProduct& OutProduct,
			std::string& OutError) -> bool;

		static auto PublishImportedProduct(
			DStaticMesh& Mesh,
			FStaticMeshBuildProduct Product,
			std::string& OutError) -> bool;

		static auto BuildCollisionProduct(
			const FStaticMeshRenderData& RenderData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			FStaticMeshCollisionBuildProduct& OutProduct,
			std::string& OutError) -> bool;
	};
}
