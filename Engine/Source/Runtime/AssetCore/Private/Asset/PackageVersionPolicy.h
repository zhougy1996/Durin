#pragma once

#include "AssetCoreAPI.h"
#include "Misc/Guid.h"

#include <array>

namespace Durin::Asset
{
	inline constexpr uint32 DastPackageMagic = 0x54534144;
	inline constexpr FGuid DastBinaryFormatId{
		0x3c59d1a9, 0x6ceb4e4c, 0xb059452d, 0xb0a5af56};
	inline constexpr std::string_view DastBinaryFormatName = "Durin.BinaryFormat.DAST";
	// Permanent wire identities for the authored- and cooked-bulk DURF branches.
	// These random GUIDs are checked in once and are never derived from the
	// diagnostic names or the historical four-byte magics.
	inline constexpr FGuid DabkBinaryFormatId{
		0x49efbbb4, 0xe2434e35, 0xa7c01c34, 0x9ed84ea0};
	inline constexpr std::string_view DabkBinaryFormatName = "Durin.BinaryFormat.DABK";
	inline constexpr FGuid DblkBinaryFormatId{
		0x76c5d46c, 0xa3744b7e, 0x9cda6c8f, 0xe0dbcd17};
	inline constexpr std::string_view DblkBinaryFormatName = "Durin.BinaryFormat.DBLK";
	inline constexpr uint32 AssetPackageV6FormatVersion = 6;
	inline constexpr uint32 AssetPackageObjectStreamVersion = 5;

	inline constexpr std::array SupportedAssetPackageReaderVersions{
		AssetPackageV6FormatVersion};

	inline constexpr uint32 OrdinaryAssetPackageWriterVersion = AssetPackageV6FormatVersion;
	// Persisted read-only caches use an explicit policy generation rather than a
	// wire version so different supported-reader sets cannot alias.
	inline constexpr uint32 AssetPackageReaderPolicyFingerprint = 0x41504304;

	constexpr auto IsSupportedAssetPackageReaderVersion(uint32 Version) -> bool
	{
		return std::ranges::find(SupportedAssetPackageReaderVersions, Version)
			!= SupportedAssetPackageReaderVersions.end();
	}

	ASSETCORE_API auto ValidateAssetPackageVersionPolicy(std::string& OutError) -> bool;
	ASSETCORE_API auto GetAssetPackageReaderPolicyIdentity() -> uint32;
}
