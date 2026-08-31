#pragma once

#include "AssetRegistry/References.h"

namespace Durin::Asset::Private
{
	struct FRegistryCacheEntry
	{
		std::string MountRoot;
		std::string RelativePath;
		std::vector<FTopLevelAssetData> TopLevelAssets;
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FAssetPath RedirectDestination;
		uint32 FormatVersion = 0;
		std::vector<FAssetPath> Dependencies;
		std::vector<FAssetPath> SoftDependencies;
		std::vector<std::string> SearchableNames;
		uint64 ObjectCount = 0;
		uint64 BulkSegmentExtent = 0;
		FXxHash128 BulkSegmentDigest;
		uint64 FileSize = 0;
		int64 LastWriteTimeTicks = 0;
	};

	auto GetMountManifest() -> std::vector<std::string>;
	auto MakeRegistryIdentity(
		std::string_view MountRoot,
		std::string_view RelativePath) -> std::string;
	auto LoadRegistryCache(
		const std::vector<std::string>& ExpectedMounts,
		std::unordered_map<std::string, FRegistryCacheEntry>& OutEntries,
		std::string& OutWarning) -> bool;
	auto WriteRegistryCache(
		const std::vector<std::string>& Mounts,
		std::vector<FRegistryCacheEntry> Entries,
		std::string& OutWarning) -> bool;
	auto BuildRegistryCacheEntries(
		const std::unordered_map<FAssetPath, FAssetData>& Assets,
		std::vector<FRegistryCacheEntry>& OutEntries,
		std::string& OutWarning) -> bool;
}
