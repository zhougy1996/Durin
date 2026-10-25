#pragma once

#include "CoreMinimal.h"
#include "Asset/AssetReadResult.h"

namespace Durin::AssetPrivate
{
	auto LoadPackageBulkBytes(std::string_view PhysicalPath, FByteBuffer& OutBytes) -> FAssetReadResult;
}
