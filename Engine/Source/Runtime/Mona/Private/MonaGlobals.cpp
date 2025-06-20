#include "MonaGlobals.h"

#include "RHI.h"

ImGuiContext* GImGuiContext = nullptr;

static auto ImGuiInit() -> void
{
	check(GDynamicRHI);
	GImGuiContext = ImGui::CreateContext();
	ImGui::SetCurrentContext(GImGuiContext);
}

auto GlfwInit() -> void
{
	glfwInit();
	// Prepare required Vulkan instance extensions
	uint32_t GlfwExtensionCount = 0;
	const char** GlfwExtensions;
	GlfwExtensions = glfwGetRequiredInstanceExtensions(&GlfwExtensionCount);
	GKleeRequiredVulkanInstanceExtensions.insert(GKleeRequiredVulkanInstanceExtensions.end(), GlfwExtensions, GlfwExtensions + GlfwExtensionCount);
}

auto KleeInit() -> void
{
	ImGuiInit();
	FKleeApplication::Create();
	FKleeApplication::Get().Initialize();
}
