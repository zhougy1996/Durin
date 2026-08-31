#pragma once

#include "AssetRegistryAPI.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/PackageFormat.h"
#include "DObject/AssetPath.h"
#include "Hash/XxHash.h"

namespace Durin::Asset
{
	struct FAssetPackageTopLevelAssetHeader
	{
		FTopLevelAssetPath AssetPath;
		std::string AssetClassName;
		FObjectPath RedirectDestination;
	};

	struct FAssetPackageHeader
	{
		FPackagePath PackagePath;
		std::vector<FAssetPackageTopLevelAssetHeader> TopLevelAssets;
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
		uint64 BytesRead = 0;
		uint64 FileBytesRead = 0;
	};

	// Reads only the bounded DAST front matter required by persistent metadata
	// discovery. This never constructs or loads package objects.
	ASSETREGISTRY_API auto ReadAssetPackageHeader(
		std::string_view PhysicalPath,
		const FAssetPath& PackagePath,
		FAssetPackageHeader& OutHeader) -> FAssetResult;

	// Supplies the mounted identity and exact raw-bulk extent required by DAST Registry validation.
	ASSETREGISTRY_API auto ReadAssetPackageHeaderBytes(
		std::span<const std::byte> FrontMatter,
		uint64 PhysicalFileBytes,
		uint64 PhysicalBulkBytes,
		const FAssetPath& PackagePath,
		FAssetPackageHeader& OutHeader) -> FAssetResult;
}
