#pragma once

#include "AssetPackageCodec.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/PackageVersionPolicy.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"

namespace Durin::Asset::Private::DastV6
{
	inline constexpr uint32 Version = AssetPackageV6FormatVersion;
	inline constexpr uint32 FormatHeaderBytes = 32;
	inline constexpr uint32 SectionEntryBytes = 48;
	inline constexpr uint32 RequiredSectionCount = 8;
	inline constexpr uint32 MaximumSectionCount = 64;
	inline constexpr uint64 MaximumHeaderBytes = 16ull * 1024ull * 1024ull;
	inline constexpr uint64 MaximumFileBytes = 1024ull * 1024ull * 1024ull;
	inline constexpr uint64 MaximumImportCount = 65'536;
	inline constexpr uint64 MaximumExportCount = 1'048'576;
	inline constexpr uint64 MaximumPayloadCount = 65'536;

	enum class ESectionKind : uint32
	{
		PublicSummary = 1,
		Import = 2,
		Name = 3,
		Type = 4,
		Schema = 5,
		Export = 6,
		Value = 7,
		PayloadDirectory = 8,
	};

	inline constexpr uint32 RequiredSectionFlag = 1;

	enum class EPayloadPlacement : uint32
	{
		ExternalDabkV1 = 1,
	};

	struct FPayloadEntry
	{
		FGuid PayloadId;
		EPayloadPlacement Placement = EPayloadPlacement::ExternalDabkV1;
		uint64 LogicalByteCount = 0;
		uint64 StoredByteCount = 0;
		FXxHash128 ContentHash;
		FXxHash128 ContainerHash;

		auto operator==(const FPayloadEntry&) const -> bool = default;
	};

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
		std::vector<FPayloadEntry> PayloadEntries;
		uint64 ExpectedImportCount = 0;
		uint64 ExpectedPayloadCount = 0;
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
		std::vector<std::byte>& OutV6Bytes) -> FAssetResult;
	ENGINE_API auto ExtractObjectStream(
		std::span<const std::byte> V6Bytes,
		std::vector<std::byte>& OutObjectStream) -> FAssetResult;

	ENGINE_API auto GetCodec() -> const FAssetPackageCodec&;
}
