#pragma once

#include "AssetSystemInternal.h"

namespace Durin::Asset::Private
{
	struct FRegistryCacheEntry
	{
		std::string MountRoot;
		std::string RelativePath;
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FAssetPath RedirectDestination;
		uint32 FormatVersion = 0;
		std::vector<FAssetPath> Dependencies;
		uint64 FileSize = 0;
		int64 LastWriteTimeTicks = 0;
	};

	struct FReferenceCacheSource
	{
		FAssetPackageFingerprint Fingerprint;
		std::vector<FAssetReferenceEdge> References;
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
	auto LoadReferenceCache(
		std::unordered_map<FAssetPath, FReferenceCacheSource>& OutSources,
		std::string& OutWarning) -> bool;
	auto WriteReferenceCache(
		const std::unordered_map<FAssetPath, FAssetPackageFingerprint>& Fingerprints,
		std::span<const FAssetReferenceEdge> References,
		std::string& OutWarning) -> bool;
}
