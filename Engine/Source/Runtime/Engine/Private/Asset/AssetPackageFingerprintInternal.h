#pragma once
#include "Asset/PackageInspection.h"

namespace Durin::AssetPrivate
{
	auto MakePackageFingerprint(std::string_view PhysicalPath, FByteView Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetReadResult;
}
