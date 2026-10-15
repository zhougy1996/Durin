#pragma once

#include "CoreMinimal.h"
#include "ShaderFormatAPI.h"

namespace Durin::ShaderFormat
{
	// Vulkan uses logical set/binding identity; no identity remap is serialized.
	SHADERFORMAT_API auto ValidateVulkanCode(FByteView Code) -> bool;
}
