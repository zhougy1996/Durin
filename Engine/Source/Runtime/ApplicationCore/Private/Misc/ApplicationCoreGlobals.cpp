#include "Misc/ApplicationCoreGlobals.h"

#include "CoreGlobals.h"
#include "Application/GenericApplication.h"
#include "ThirdParty/Glfw/GlfwCommon.h"

namespace Durin
{
	std::shared_ptr<FGenericApplication> GApp = nullptr;

	auto ApplicationCoreInit() -> void
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

	auto ApplicationCoreShutdown() -> void
	{
		glfwTerminate();
	}
}
