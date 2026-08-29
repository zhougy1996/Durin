#pragma once

#include "AssetRegistry/Publication.h"

#define DURIN_ENGINE_ASSET_INTERNAL 1
#include "Asset/Redirector.h"
#include "Asset/Mutation.h"
#include "Asset/Testing.h"
#undef DURIN_ENGINE_ASSET_INTERNAL

namespace Durin::Asset
{
	struct FAssetRelocationState;
	struct FAssetRedirectorFixupState;
	class FAssetLoadService;
	class FAssetMutationCoordinator;

	struct FAssetPublicationState
	{
		std::unordered_map<FAssetPath, FAssetData> Assets;
		std::vector<FAssetReferenceEdge> ReferenceEdges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> ReferenceFingerprints;
		std::vector<FAssetResult> ReferenceErrors;
		FAssetReferenceIndexStats ReferenceStats;
		std::string ReferenceCacheWarning;
		bool bReferenceIndexComplete = false;
	};

	class FAssetPublicationCoordinator
	{
	public:
		auto CapturePreparedState() const -> FAssetPublicationState;
		auto PublishPreparedState(uint64 ExpectedRevision,
			FAssetPublicationState State) -> FAssetResult;
		auto PublishAssetMetadata(FAssetData Data) -> FAssetResult;
		auto PublishAssetMetadataBatch(std::vector<FAssetData> Assets) -> FAssetResult;

	};

	ENGINE_API auto GetAssetPublicationCoordinator() -> FAssetPublicationCoordinator&;
}
