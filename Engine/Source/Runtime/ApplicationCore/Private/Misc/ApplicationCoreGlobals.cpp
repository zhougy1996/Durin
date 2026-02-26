#include "Misc/ApplicationCoreGlobals.h"

#include "CoreGlobals.h"
#include "Application/GenericApplication.h"

namespace Doge
{
	TSharedPtr<FGenericApplication> GApp = nullptr;

	auto ApplicationInit() -> void
	{
		#ifdef _WIN32
		SetConsoleOutputCP(CP_UTF8);
		#endif
		glfwInit();
		// Prepare required Vulkan instance extensions
		uint32_t GlfwExtensionCount = 0;
		const char** GlfwExtensions;
		GlfwExtensions = glfwGetRequiredInstanceExtensions(&GlfwExtensionCount);
		GMonaRequiredVulkanInstanceExtensions.insert(GMonaRequiredVulkanInstanceExtensions.end(), GlfwExtensions, GlfwExtensions + GlfwExtensionCount);
	}
}