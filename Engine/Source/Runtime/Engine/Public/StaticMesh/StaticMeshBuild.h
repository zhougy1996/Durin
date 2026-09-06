#pragma once

#include "DerivedDataCacheKeyProxy.h"
#include "EngineAPI.h"
#include "StaticMesh/StaticMeshBuildProvider.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	// Immutable object facts captured before StaticMesh recipe work begins.
	struct FStaticMeshReconciliationSnapshot
	{
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		float NormalizedSize = 1.5f;
	};

	// Detached Engine request; cache policy is not forwarded to recipe code.
	struct FStaticMeshBuildRequest
	{
		FStaticMeshReconciliationSnapshot Reconciliation;
		FStaticMeshImportedData ImportedData;
		bool bPersistDerivedData = true;
	};

	enum class EStaticMeshBuildOrigin : uint8
	{
		CacheHit,
		Rebuilt
	};

	// Caller-owned observation; applying its values never installs this history.
	struct FStaticMeshBuildResult
	{
		std::unique_ptr<FStaticMeshRenderData> RenderData;
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		float NormalizedSize = 1.5f;
		FCacheKeyProxy DerivedDataKey;
		bool bSlotMetadataChanged = false;
		EStaticMeshBuildOrigin Origin = EStaticMeshBuildOrigin::Rebuilt;
		FStaticMeshBuildProviderDescriptor Descriptor;
		uint64 CacheReadNanoseconds = 0;
		uint64 CacheWriteNanoseconds = 0;
		uint64 PayloadBytes = 0;
		std::string DiagnosticMessage;
	};

	// Detached collision geometry plus the current Engine operation's observation.
	struct FStaticMeshCollisionBuildResult
	{
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
		EStaticMeshBuildOrigin Origin = EStaticMeshBuildOrigin::Rebuilt;
		FCacheKeyProxy DerivedDataKey;
		std::string Diagnostic;
		uint64 PayloadBytes = 0;
		FStaticMeshBuildProviderDescriptor Descriptor;
		uint64 CacheReadNanoseconds = 0;
		uint64 CacheWriteNanoseconds = 0;
	};

	ENGINE_API auto BuildStaticMeshDerivedData(
		FStaticMeshBuildRequest Request,
		FStaticMeshBuildResult& OutProduct,
		std::string& OutError) -> bool;
	ENGINE_API auto BuildStaticMeshCollisionDerivedData(
		const FStaticMeshRenderData& RenderData,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FStaticMeshCollisionBuildResult& OutProduct,
		std::string& OutError,
		bool bPersistDerivedData = true) -> bool;

	// Capture on the asset's owner thread before dispatching detached work.
	ENGINE_API auto CaptureStaticMeshReconciliation(const DStaticMesh& Mesh)
		-> FStaticMeshReconciliationSnapshot;
	// Applies on the owner thread; candidate failure preserves existing resources.
	ENGINE_API auto ApplyStaticMeshBuildResult(DStaticMesh& Mesh,
		FStaticMeshImportedData Source, FStaticMeshBuildResult Product, std::string& OutError,
		bool bMarkPackageDirty = true) -> bool;
	ENGINE_API auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		const FStaticMeshImportedData& ImportedData,
		std::string& OutError) -> bool;
	// Fresh authored input boundary: capture once, build from seeded residency, then release.
	ENGINE_API auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		FStaticMeshDecodedGeometry Geometry,
		std::string& OutError) -> bool;
}
