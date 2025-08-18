#include "Misc/ApplicationCoreGlobals.h"

#include "CoreGlobals.h"

auto ApplicationInit() -> void
{
	SetConsoleOutputCP(CP_UTF8);
	glfwInit();
	// Prepare required Vulkan instance extensions
	uint32_t GlfwExtensionCount = 0;
	const char** GlfwExtensions;
	GlfwExtensions = glfwGetRequiredInstanceExtensions(&GlfwExtensionCount);
	GMonaRequiredVulkanInstanceExtensions.insert(GMonaRequiredVulkanInstanceExtensions.end(), GlfwExtensions, GlfwExtensions + GlfwExtensionCount);
}