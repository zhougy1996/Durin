#pragma once

#include <expected>

#include "EngineAPI.h"
#include "StaticMesh/StaticMeshBuildDiagnostics.h"
#include "StaticMesh/StaticMeshBuildFailure.h"
#include "StaticMesh/StaticMeshBuildProvider.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	// Immutable object facts captured before StaticMesh recipe work begins.
	struct FStaticMeshReconciliationSnapshot
	{
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		float NormalizedSize = 1.5f;
		FXxHash128 SourceIdentity;
	};

	// Detached Engine request; cache policy is not forwarded to recipe code.
	struct FStaticMeshBuildRequest
	{
		FStaticMeshReconciliationSnapshot Reconciliation;
		FStaticMeshSource Source;
		bool bPersistDerivedData = true;
	};

	// Advanced detached building API. Ordinary asset callers use DStaticMesh::Build/AsyncBuild.
	class FStaticMeshBuilder
	{
	public:
		ENGINE_API static auto Build(
			FStaticMeshBuildRequest Request,
			const FAssetBuildTaskContext& Control = {},
			std::vector<FStaticMeshCacheError>* OutCacheErrors = nullptr, uint64 ExpectedProviderRegistration = 0) -> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>;
		// Validate and finish detached CPU data before publication, including non-recipe inputs.
		ENGINE_API static auto FinalizeRenderData(FStaticMeshRenderData& Render,
			const FAssetBuildTaskContext& Control = {}) -> std::expected<void, FStaticMeshBuildFailure>;
		ENGINE_API static auto Capture(const DStaticMesh& Mesh)
			-> FStaticMeshReconciliationSnapshot;
	};

	// Accept source, slots, provenance and finalized render data as one asset transaction.
	// Collision invalidation/admission belongs to this boundary, not render publication.
	// Replace a finalized render projection of the current accepted geometry.
	// Source and physics resources remain unchanged; geometry edits use CommitStaticMeshBuild.
	ENGINE_API auto PublishStaticMeshRenderData(DStaticMesh& Mesh,
		std::unique_ptr<FStaticMeshRenderData> Render) -> std::expected<void, FStaticMeshBuildFailure>;

	ENGINE_API auto CommitStaticMeshBuild(DStaticMesh& Mesh,
		std::unique_ptr<FStaticMeshRenderData> Render, FStaticMeshSource Source,
		const FStaticMeshReconciliationSnapshot& Snapshot,
		bool bMarkPackageDirty = true, const FAssetBuildTaskContext& Control = {},
		DAssetImportData* PreparedImportData = nullptr,
		std::vector<FMeshMaterialSlotDefinition>* PreparedMaterialSlots = nullptr,
		bool bPersistCollisionDerivedData = true) -> std::expected<void, FStaticMeshBuildFailure>;
}
