#pragma once

#include "AssetRegistry/Publication.h"

#define DURIN_ENGINE_ASSET_INTERNAL 1
#include "Asset/Redirector.h"
#include "Asset/Mutation.h"
#include "Asset/Testing.h"
#undef DURIN_ENGINE_ASSET_INTERNAL

namespace Durin
{
	struct FAssetRelocationState;
	struct FAssetRedirectorFixupState;
	class FAssetLoadService;
	class FAssetMutationCoordinator;

	struct FAssetPublicationState
	{
		std::unordered_map<FPackagePath, FAssetData> Assets;
		std::vector<FAssetPackageReferenceEdge> ReferenceEdges;
		std::unordered_map<FPackagePath, FAssetPackageFingerprint> ReferenceFingerprints;
		std::vector<FAssetRegistryResult> ReferenceErrors;
		bool bReferenceIndexComplete = false;
	};

	auto BuildAssetPackageReferenceProjection(
		const std::unordered_map<FPackagePath, FAssetData>& Assets,
		std::vector<FAssetPackageReferenceEdge>& OutEdges,
		std::unordered_map<FPackagePath, FAssetPackageFingerprint>& OutFingerprints)
		-> FAssetResult;

	class FAssetPublicationCoordinator
	{
	public:
		auto CapturePreparedState() const -> FAssetPublicationState;
		auto PublishDelta(FAssetRegistryDelta Delta) -> FAssetResult;
		auto ReconcileProjection(std::span<const FPackagePath> Paths)
			-> FAssetResult;
		auto PublishAssetMetadata(FAssetData Data) -> FAssetResult;
		auto PublishAssetMetadataBatch(std::vector<FAssetData> Assets) -> FAssetResult;

	};

	ENGINE_API auto GetAssetPublicationCoordinator() -> FAssetPublicationCoordinator&;
}
