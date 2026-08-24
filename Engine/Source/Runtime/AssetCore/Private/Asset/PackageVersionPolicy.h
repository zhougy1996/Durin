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
	inline constexpr uint32 AssetPackageV5FormatVersion = 5;
	inline constexpr uint32 AssetPackageV6FormatVersion = 6;
	inline constexpr uint32 AssetPackageObjectStreamVersion = AssetPackageV5FormatVersion;

	inline constexpr std::array SupportedAssetPackageReaderVersions{
		AssetPackageV5FormatVersion};

	inline constexpr uint32 OrdinaryAssetPackageWriterVersion = AssetPackageV5FormatVersion;
	// Persisted read-only caches use an explicit policy generation rather than a
	// wire version so different supported-reader sets cannot alias.
	inline constexpr uint32 AssetPackageReaderPolicyFingerprint = 0x41504303;

	constexpr auto IsSupportedAssetPackageReaderVersion(uint32 Version) -> bool
	{
		return std::ranges::find(SupportedAssetPackageReaderVersions, Version)
			!= SupportedAssetPackageReaderVersions.end();
	}

	ASSETCORE_API auto ValidateAssetPackageVersionPolicy(std::string& OutError) -> bool;
	ASSETCORE_API auto GetAssetPackageReaderPolicyIdentity() -> uint32;
}
