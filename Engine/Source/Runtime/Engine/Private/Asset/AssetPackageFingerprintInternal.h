#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "Asset/PackageInspection.h"

namespace Durin::AssetPrivate
{
	auto MakePackageFingerprint(std::string_view PhysicalPath, FByteView Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> std::expected<void, Durin::UnifiedError::FError>;
}
