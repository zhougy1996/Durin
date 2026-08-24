#pragma once

#include "AssetPackageCodec.h"
#include "Asset/PackageVersionPolicy.h"

namespace Durin::Asset::Private::DastV5
{
	inline constexpr uint32 Version = AssetPackageV5FormatVersion;
	auto GetCodec() -> const FAssetPackageCodec&;
	ASSETCORE_API auto BuildPackageFromObjectStream(
		std::span<const std::byte> ObjectStreamBytes,
		std::vector<std::byte>& OutV5Bytes) -> FAssetResult;
}
