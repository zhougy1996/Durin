#pragma once

#include "AssetBuild/BuildTypes.h"
#include "DerivedDataObjectStore.h"

namespace Durin::AssetBuild
{
	enum class EBuildCacheQueryStatus : uint8
	{
		Hit,
		Missing,
		StorageError,
		Skipped
	};

	// Opaque cache result; recipe modules retain all family compatibility interpretation.
	struct FBuildCacheQueryResult
	{
		EBuildCacheQueryStatus Status = EBuildCacheQueryStatus::Skipped;
		FBuildValue Value;
		std::string Diagnostic;
	};

	// Maps opaque keys and immutable values to an existing family-neutral object store.
	class FBuildCacheClient
	{
	public:
		ASSETBUILDCORE_API explicit FBuildCacheClient(
			Asset::FDerivedDataObjectStore& InStore);

		ASSETBUILDCORE_API auto Query(
			std::string_view Key, std::string ValueName,
			const FBuildPolicy& Policy) const -> FBuildCacheQueryResult;
		ASSETBUILDCORE_API auto Store(
			std::string_view Key, const FBuildValue& Value,
			const FBuildPolicy& Policy, std::string* OutError = nullptr) const -> bool;

	private:
		Asset::FDerivedDataObjectStore* StoreTarget = nullptr;
	};
}
