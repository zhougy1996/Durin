#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Asset/AssetBuildTaskContext.h"
#include "StaticMesh/StaticMeshBuildFailure.h"
#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	// Immutable object facts captured before StaticMesh build work begins.
	struct FStaticMeshReconciliationSnapshot
	{
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		float NormalizedSize = 1.5f;
		FXxHash128 SourceIdentity;
	};

#if DURIN_WITH_EDITORONLY_DATA
	// Detached Engine request; cache policy is not forwarded to build code.
	struct FStaticMeshBuildRequest
	{
		FStaticMeshBuildSettings Settings;
		FStaticMeshSource Source;
		bool bPersistDerivedData = true;
	};

	ENGINE_API auto MakeStaticMeshBuildSettings(std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		float NormalizedSize = 1.5f) -> FStaticMeshBuildSettings;

	// Worker-safe execution using the resident MeshBuilder module.
	ENGINE_API auto BuildStaticMeshRenderData(FStaticMeshBuildRequest Request,
		const FAssetBuildTaskContext& Control = {})
		-> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>;
#endif

#if DURIN_WITH_EDITORONLY_DATA
	ENGINE_API auto CaptureStaticMeshReconciliation(const DStaticMesh& Mesh)
		-> FStaticMeshReconciliationSnapshot;

#endif

	// Accept source, slots, provenance and finalized render data as one asset transaction.
	// Collision invalidation/admission belongs to this boundary, not render publication.
	// Replace a finalized render projection of the current accepted geometry.
	// Source and physics resources remain unchanged; geometry edits use CommitStaticMeshBuild.
	ENGINE_API auto PublishStaticMeshRenderData(DStaticMesh& Mesh,
		std::unique_ptr<FStaticMeshRenderData> Render) -> std::expected<void, FStaticMeshBuildFailure>;

#if DURIN_WITH_EDITORONLY_DATA
	ENGINE_API auto CommitStaticMeshBuild(DStaticMesh& Mesh,
		std::unique_ptr<FStaticMeshRenderData> Render, FStaticMeshSource Source,
		const FStaticMeshReconciliationSnapshot& Snapshot,
		bool bMarkPackageDirty = true, const FAssetBuildTaskContext& Control = {},
		DAssetImportData* PreparedImportData = nullptr,
		std::vector<FMeshMaterialSlotDefinition>* PreparedMaterialSlots = nullptr,
		bool bPersistCollisionDerivedData = true) -> std::expected<void, FStaticMeshBuildFailure>;
#endif

}
