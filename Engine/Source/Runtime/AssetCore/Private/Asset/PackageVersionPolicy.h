#pragma once

#include "AssetCoreAPI.h"

#include <array>

namespace Durin::Asset
{
	inline constexpr uint32 DastPackageMagic = 0x54534144;
	inline constexpr uint32 AssetPackageV5FormatVersion = 5;
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
