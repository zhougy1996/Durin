#include "ApplicationCoreGlobals.h"

#include "Application/GenericApplication.h"
#include "Misc/GlfwVulkanInitialization.h"
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

		auto DiscoverVulkanSurfaceRequirements()
			-> FVulkanSurfaceRequirementsResult
		{
			FGlfwVulkanExtensionQueryResult Result =
				QueryRequiredGlfwVulkanInstanceExtensions(
					[](uint32_t* Count) {
						return glfwGetRequiredInstanceExtensions(Count);
					},
					[](const char** Description) {
						return glfwGetError(Description);
					});
			return {
				.RequiredInstanceExtensions = std::move(Result.Extensions),
				.Diagnostic = std::move(Result.Diagnostic)};
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
		GApplicationCoreInitializationCount = 1;
		return true;
	}

	auto IsApplicationCoreInitialized() -> bool
	{
		return GApplicationCoreInitializationCount > 0;
	}

	auto GetVulkanSurfaceRequirements()
		-> FVulkanSurfaceRequirementsResult
	{
		if (!IsApplicationCoreInitialized())
		{
			return {
				.Diagnostic =
					"Vulkan surface requirements were queried before ApplicationCore initialization."};
		}

		return DiscoverVulkanSurfaceRequirements();
	}

	auto ShutdownApplicationCore() -> void
	{
		if (GApplicationCoreInitializationCount == 0) return;
		if (--GApplicationCoreInitializationCount > 0) return;
		DestroyGlfwCursors();
		glfwTerminate();
	}
}
