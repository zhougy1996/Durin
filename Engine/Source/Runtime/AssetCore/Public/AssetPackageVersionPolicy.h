#pragma once

#include "AssetCoreAPI.h"

#include <array>

namespace Durin::Asset
{
	inline constexpr uint32 DastPackageMagic = 0x54534144;
	inline constexpr uint32 AssetPackageV3FormatVersion = 3;
	inline constexpr uint32 AssetPackageV4FormatVersion = 4;

	inline constexpr std::array SupportedAssetPackageReaderVersions{
		AssetPackageV3FormatVersion,
		AssetPackageV4FormatVersion};

	inline constexpr uint32 LatestAssetPackageWriterVersion = AssetPackageV4FormatVersion;
	inline constexpr uint32 OrdinaryAssetPackageWriterVersion = AssetPackageV3FormatVersion;
	inline constexpr uint32 AssetPackageMigrationWriterVersion = LatestAssetPackageWriterVersion;
	// Persisted read-only caches use this identity so changing the supported
	// reader set invalidates entries even when package timestamps and sizes match.
	inline constexpr uint32 AssetPackageReaderPolicyFingerprint =
		(AssetPackageV3FormatVersion << 16) | AssetPackageV4FormatVersion;

	enum class EAssetPackageReaderKind : uint8
	{
		Unsupported,
		DastV3,
		DastV4,
	};

	constexpr auto SelectAssetPackageReader(uint32 Version) -> EAssetPackageReaderKind
	{
		switch (Version)
		{
		case AssetPackageV3FormatVersion: return EAssetPackageReaderKind::DastV3;
		case AssetPackageV4FormatVersion: return EAssetPackageReaderKind::DastV4;
		default: return EAssetPackageReaderKind::Unsupported;
		}
	}

	constexpr auto IsSupportedAssetPackageReaderVersion(uint32 Version) -> bool
	{
		return SelectAssetPackageReader(Version) != EAssetPackageReaderKind::Unsupported;
	}
}
