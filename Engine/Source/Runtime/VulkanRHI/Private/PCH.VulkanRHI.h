#pragma once

#include "VulkanRHI/Definitions.h"

// Vulkan
#ifdef _WIN32
	#define VK_USE_PLATFORM_WIN32_KHR
#elif  defined __APPLE__
	#define VK_USE_PLATFORM_MACOS_MVK
	#define VK_USE_PLATFORM_METAL_EXT
#endif
#include "vulkan/vulkan.hpp"


#include "CoreMinimal.h"
#include "RHIFwd.h"


