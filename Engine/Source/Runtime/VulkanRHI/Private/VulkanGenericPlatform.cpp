#include "VulkanGenericPlatform.h"

#include "Misc/ApplicationCoreGlobals.h"
#include "Application/GenericApplication.h"
#include "Window/GenericWindow.h"

namespace Doge::VulkanRHI
{
	auto FVulkanGenericPlatform::CreateSurface(void* InWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR
	{
		auto Window = GApp->FindWindowByNativeWindowHandle(InWindowHandle);

		void* Surface = Window->CreateVulkanSurface(Instance);
		auto RawSurface = static_cast<VkSurfaceKHR>(Surface);

		return vk::SurfaceKHR(RawSurface);
	}
}