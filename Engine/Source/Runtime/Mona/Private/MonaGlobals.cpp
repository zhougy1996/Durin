#include "MonaGlobals.h"

#include "RHI.h"
#include "Application/MonaApplication.h"

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
	GMonaRequiredVulkanInstanceExtensions.insert(GMonaRequiredVulkanInstanceExtensions.end(), GlfwExtensions, GlfwExtensions + GlfwExtensionCount);
}

auto MonaInit() -> void
{
	ImGuiInit();
	FMonaApplication::Create();
	FMonaApplication::Get().Initialize();
}
