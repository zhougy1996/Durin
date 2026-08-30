#pragma once

#include "AssetPackageCodec.h"
#include "Asset/PackageBulkData.h"
#include "AssetRegistry/Catalog.h"

namespace Durin::Asset::Private::DastV7
{
	inline constexpr uint32 FormatHeaderBytes = 32;
	inline constexpr uint32 SectionEntryBytes = 48;
	inline constexpr uint32 RequiredSectionCount = 8;
	inline constexpr uint32 MaximumSectionCount = 64;
	inline constexpr uint64 MaximumHeaderBytes = 16ull * 1024ull * 1024ull;
	inline constexpr uint64 MaximumFileBytes = 1024ull * 1024ull * 1024ull;
	inline constexpr uint64 MaximumImportCount = 65'536;
	inline constexpr uint64 MaximumExportCount = 1'048'576;
	inline constexpr uint64 MaximumPayloadCount = 65'536;
	inline constexpr uint32 RequiredSectionFlag = 1;

	struct FSectionEntry
	{
		uint32 Kind = 0;
		uint32 Flags = 0;
		uint64 Offset = 0;
		uint64 Size = 0;
		FXxHash128 Hash;
	};

	struct FParsedPackage
	{
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		uint32 MainExportIndex = 0;
		std::string AssetClass;
		std::string RedirectDestination;
		std::vector<std::string> Imports;
		uint64 ExportCount = 0;
		std::vector<FPackageBulkDataEntry> BulkEntries;
		FPackageBulkSegmentSummary BulkSegment;
		std::array<FSectionEntry, RequiredSectionCount> RequiredEntries;
		std::array<std::span<const std::byte>, RequiredSectionCount> RequiredSections;
		bool bHasUnknownSkippableSections = false;
		uint64 HeaderBytes = 0;
	};

	ENGINE_API auto ParsePackage(
		std::span<const std::byte> Bytes,
		FParsedPackage& OutPackage,
		std::string* OutError = nullptr) -> bool;

	ENGINE_API auto BuildPackageFromObjectStream(
		std::span<const std::byte> ObjectStreamBytes,
		std::vector<std::byte>& OutBytes) -> FAssetResult;
	ENGINE_API auto ExtractObjectStream(
		std::span<const std::byte> Bytes,
		std::vector<std::byte>& OutObjectStream) -> FAssetResult;

	ENGINE_API auto GetCodec() -> const FAssetPackageCodec&;
}
