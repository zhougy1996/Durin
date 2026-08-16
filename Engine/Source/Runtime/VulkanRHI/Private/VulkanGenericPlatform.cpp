#include "VulkanGenericPlatform.h"

#include "ApplicationCore.h"
#include "Application/GenericApplication.h"
#include "Window/GenericWindow.h"

namespace Durin::VulkanRHI
{
	auto FVulkanGenericPlatform::CreateSurface(void* InWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR
	{
		auto Window = GApp->FindWindowByNativeWindowHandle(InWindowHandle);
		if (!Window)
		{
			DURIN_ERROR(
				"Vulkan surface creation failed because the native window is not registered with the active application.");
			return VK_NULL_HANDLE;
		}

		void* Surface = Window->CreateVulkanSurface(Instance);
		if (!Surface) return VK_NULL_HANDLE;
		auto RawSurface = static_cast<VkSurfaceKHR>(Surface);

		return vk::SurfaceKHR(RawSurface);
	}
}
