#include "ApplicationCoreGlobals.h"

#include "CoreGlobals.h"
#include "Application/GenericApplication.h"
#include "ThirdParty/Glfw/GlfwCommon.h"
#include "Window/GlfwWindow.h"

namespace Durin
{
	namespace
	{
		auto ConfigureConsoleOutputEncoding() -> void
		{
			#ifdef _WIN32
			SetConsoleOutputCP(CP_UTF8);
			#endif
		}

		auto AppendRequiredGlfwVulkanInstanceExtensions() -> void
		{
			// Collect the platform extensions GLFW needs before Vulkan startup.
			uint32_t GlfwExtensionCount = 0;
			const char** GlfwExtensions = glfwGetRequiredInstanceExtensions(&GlfwExtensionCount);
			GMonaRequiredVulkanInstanceExtensions.insert(GMonaRequiredVulkanInstanceExtensions.end(), GlfwExtensions, GlfwExtensions + GlfwExtensionCount);
		}
	}

	std::shared_ptr<FGenericApplication> GApp = nullptr;

	auto InitializeApplicationCore() -> void
	{
		ConfigureConsoleOutputEncoding();
		glfwInit();
		InitGlfwCursors();
		AppendRequiredGlfwVulkanInstanceExtensions();
	}

	auto ShutdownApplicationCore() -> void
	{
		DestroyGlfwCursors();
		glfwTerminate();
	}
}
