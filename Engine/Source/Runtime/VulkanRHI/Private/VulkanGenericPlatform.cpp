#include "VulkanGenericPlatform.h"

#include "Misc/ApplicationCoreGlobals.h"
#include "Application/GenericApplication.h"
#include "Window/GenericWindow.h"

auto FVulkanGenericPlatform::CreateSurface(void* InWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR
{
	auto Window = GApp->FindWindowByNativeWindowHandle(InWindowHandle);

	void* Surface = Window->CreateVulkanSurface(static_cast<void*>(Instance));
	VkSurfaceKHR RawSurface = static_cast<VkSurfaceKHR>(Surface);

	return vk::SurfaceKHR(RawSurface);
}