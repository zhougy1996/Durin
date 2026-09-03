#pragma once

#define DURIN_ENGINE_ASSET_INTERNAL 1
#include "Asset/Mutation.h"
#undef DURIN_ENGINE_ASSET_INTERNAL

namespace Durin::AssetPrivate
{
	// Owns process-local persistent-reference providers and advances its revision
	// whenever provider availability changes.
	struct FAssetReferenceStoreRegistry
	{
		std::map<FAssetReferenceStoreHandle, IAssetReferenceStore*> Stores;
		FAssetReferenceStoreHandle NextHandle = 1;
		uint64 Revision = 1;
	};

	auto GetAssetReferenceStoreRegistry() -> FAssetReferenceStoreRegistry&;
	auto GetAssetReferenceStoreRevision() -> uint64;
}
