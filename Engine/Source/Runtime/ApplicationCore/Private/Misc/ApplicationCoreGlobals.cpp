#include "ApplicationCoreGlobals.h"

#include "CoreGlobals.h"
#include "Application/GenericApplication.h"
#include "ThirdParty/Glfw/GlfwCommon.h"
#include "Window/GlfwWindow.h"

namespace Durin
{
	namespace
	{
		uint32 GApplicationCoreInitializationCount = 0;
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

	auto InitializeApplicationCore() -> bool
	{
		if (GApplicationCoreInitializationCount > 0)
		{
			++GApplicationCoreInitializationCount;
			return true;
		}
		ConfigureConsoleOutputEncoding();
		if (glfwInit() != GLFW_TRUE)
		{
			const char* Description = nullptr;
			const int Error = glfwGetError(&Description);
			DURIN_ERROR("GLFW initialization failed ({}): {}.", Error,
				Description ? Description : "No native diagnostic was provided");
			return false;
		}
		InitGlfwCursors();
		AppendRequiredGlfwVulkanInstanceExtensions();
		GApplicationCoreInitializationCount = 1;
		return true;
	}

	auto IsApplicationCoreInitialized() -> bool
	{
		return GApplicationCoreInitializationCount > 0;
	}

	auto ShutdownApplicationCore() -> void
	{
		if (GApplicationCoreInitializationCount == 0) return;
		if (--GApplicationCoreInitializationCount > 0) return;
		DestroyGlfwCursors();
		glfwTerminate();
		GMonaRequiredVulkanInstanceExtensions.clear();
	}
}
