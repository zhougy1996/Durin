#pragma once

#include "CoreGlobals.h"

inline auto ApplicationInit() -> void
{
	glfwInit();
	// Prepare required Vulkan instance extensions
	uint32_t GlfwExtensionCount = 0;
	const char** GlfwExtensions;
	GlfwExtensions = glfwGetRequiredInstanceExtensions(&GlfwExtensionCount);
	GMonaRequiredVulkanInstanceExtensions.insert(GMonaRequiredVulkanInstanceExtensions.end(), GlfwExtensions, GlfwExtensions + GlfwExtensionCount);
}