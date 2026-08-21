#pragma once

#include "AssetMutationTransactionInternal.h"

namespace Durin::Asset::Private
{
	// Owns process-local persistent-reference providers and advances its revision
	// whenever provider availability changes.
	struct FAssetReferenceStoreRegistry
	{
		struct FEntry
		{
			FModuleOwnedResourceLease OwnerResource;
			IAssetReferenceStore* Store = nullptr;
			FModuleOwnedCallbackGate OwnerGate;
		};

		std::map<FAssetReferenceStoreHandle, FEntry> Stores;
		FAssetReferenceStoreHandle NextHandle = 1;
		uint64 Revision = 1;
	};

	auto GetAssetReferenceStoreRegistry() -> FAssetReferenceStoreRegistry&;
	auto GetAssetReferenceStoreRevision() -> uint64;
	auto AppendRegisteredReferenceStoreDeletionProjection(
		std::span<const FAssetPath> Paths,
		std::vector<FAssetDeletionBatchWarning>& OutWarnings,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> void;
}
