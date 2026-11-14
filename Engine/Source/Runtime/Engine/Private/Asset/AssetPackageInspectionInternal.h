#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

namespace Durin::AssetPrivate
{
	auto LoadPackageBulkBytes(std::string_view PhysicalPath, FByteBuffer& OutBytes) -> std::expected<void, Durin::UnifiedError::FError>;
}
