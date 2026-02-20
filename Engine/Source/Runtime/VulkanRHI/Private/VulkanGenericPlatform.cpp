#include "VulkanGenericPlatform.h"

#include "ThirdParty/Glfw/GlfwCommon.h"

auto FVulkanGenericPlatform::CreateSurface(void* GlfwWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR
{
	auto* GlfwWindow = static_cast<GLFWwindow*>(GlfwWindowHandle);

	VkSurfaceKHR Surface;
	if (glfwCreateWindowSurface(Instance, GlfwWindow, nullptr, &Surface) != VK_SUCCESS)
	{
		DOGE_ERROR("Failed to create window surface.");
		return nullptr;
	}

	return Surface;
}