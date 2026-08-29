#pragma once

#include "AssetRegistryAPI.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/PackageFormat.h"
#include "DObject/AssetPath.h"

namespace Durin::Asset
{
	struct FAssetPackageHeader
	{
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FAssetPath RedirectDestination;
		uint32 FormatVersion = 0;
		std::vector<FAssetPath> Dependencies;
		uint64 ObjectCount = 0;
		uint64 BytesRead = 0;
		uint64 FileBytesRead = 0;
	};

	// Reads only the bounded DAST front matter required by persistent metadata
	// discovery. This never constructs or loads package objects.
	ASSETREGISTRY_API auto ReadAssetPackageHeader(
		std::string_view PhysicalPath,
		FAssetPackageHeader& OutHeader) -> FAssetResult;

	ASSETREGISTRY_API auto ReadAssetPackageHeaderBytes(
		std::span<const std::byte> FrontMatter,
		uint64 PhysicalFileBytes,
		FAssetPackageHeader& OutHeader) -> FAssetResult;
}

namespace Durin::Asset::Dast
{
	struct FPublicSummary
	{
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		uint32 MainExportIndex = 0;
		std::string AssetClass;
		std::string RedirectDestination;
		std::vector<std::string> Imports;
		uint64 ExportCount = 0;
		uint64 PayloadCount = 0;
	};

	// Canonical decoder shared by registry header inspection and Engine's full
	// DAST reader. The two sections must already have passed envelope bounds and
	// hash validation.
	ASSETREGISTRY_API auto DecodePublicSummary(
		std::span<const std::byte> PublicSummaryBytes,
		std::span<const std::byte> ImportBytes,
		EAssetRegistryEntryKind EntryKind,
		FPublicSummary& OutSummary,
		std::string* OutError = nullptr) -> bool;
}
