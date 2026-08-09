#pragma once

#include "AssetCoreAPI.h"

#include <array>

namespace Durin::Asset
{
	inline constexpr uint32 DastPackageMagic = 0x54534144;
	inline constexpr uint32 AssetPackageV4FormatVersion = 4;

	inline constexpr std::array SupportedAssetPackageReaderVersions{
		AssetPackageV4FormatVersion};

	inline constexpr uint32 LatestAssetPackageWriterVersion = AssetPackageV4FormatVersion;
	inline constexpr uint32 OrdinaryAssetPackageWriterVersion = LatestAssetPackageWriterVersion;
	inline constexpr uint32 AssetPackageMigrationWriterVersion = LatestAssetPackageWriterVersion;
	// Persisted read-only caches use an explicit policy generation rather than a
	// wire version so different supported-reader sets cannot alias.
	inline constexpr uint32 AssetPackageReaderPolicyFingerprint = 0x41504301;
	inline constexpr uint32 SyntheticAssetPackageFormatVersionForTesting = 0xffff0004;

	constexpr auto IsSupportedAssetPackageReaderVersion(uint32 Version) -> bool
	{
		return std::ranges::find(SupportedAssetPackageReaderVersions, Version)
			!= SupportedAssetPackageReaderVersions.end();
	}

	ASSETCORE_API auto ValidateAssetPackageVersionPolicy(std::string& OutError) -> bool;
	ASSETCORE_API auto GetAssetPackageReaderPolicyIdentity() -> uint32;

	// Installs the permanent synthetic reader used to qualify shared dispatch and migration.
	class FScopedSyntheticAssetPackageCodecForTesting
	{
	public:
		ASSETCORE_API FScopedSyntheticAssetPackageCodecForTesting();
		ASSETCORE_API ~FScopedSyntheticAssetPackageCodecForTesting();
		FScopedSyntheticAssetPackageCodecForTesting(
			const FScopedSyntheticAssetPackageCodecForTesting&) = delete;
		auto operator=(const FScopedSyntheticAssetPackageCodecForTesting&)
			-> FScopedSyntheticAssetPackageCodecForTesting& = delete;
	};
}
