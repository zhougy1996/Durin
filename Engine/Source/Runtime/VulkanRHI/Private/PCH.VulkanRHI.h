#pragma once

#define MODULE_NAME "VulkanRHI"

#include "CoreMinimal.h"

// Vulkan
#ifdef _WIN32
	#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "vulkan/vulkan.hpp"

#include "Definitions.VulkanRHI.h"
