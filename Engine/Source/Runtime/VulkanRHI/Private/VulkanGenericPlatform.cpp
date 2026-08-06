#include "VulkanGenericPlatform.h"

#include "ApplicationCore.h"
#include "Application/GenericApplication.h"
#include "Window/GenericWindow.h"

namespace Durin::VulkanRHI
{
	auto FVulkanGenericPlatform::CreateSurface(void* InWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR
	{
		auto Window = GApp->FindWindowByNativeWindowHandle(InWindowHandle);
		check(Window != nullptr);

		void* Surface = Window->CreateVulkanSurface(Instance);
		auto RawSurface = static_cast<VkSurfaceKHR>(Surface);

		return vk::SurfaceKHR(RawSurface);
	}
}
