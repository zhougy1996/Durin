#pragma once

// Vulkan
#ifdef _WIN32
	#define VK_USE_PLATFORM_WIN32_KHR
#elif  defined __APPLE__
	#define VK_USE_PLATFORM_MACOS_MVK
	#define VK_USE_PLATFORM_METAL_EXT
#endif

// Use dynamic dispatch loader to load Vulkan functions at runtime.
#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
	#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#define NOMINMAX
#include "vulkan/vulkan.hpp"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vma/vk_mem_alloc.h"

#include "CoreMinimal.h"
#include "RHI.h"


