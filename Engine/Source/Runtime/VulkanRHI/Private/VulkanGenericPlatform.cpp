#include "VulkanGenericPlatform.h"
#include "VulkanRHIPrivate.h"

#include "ApplicationCore.h"
#include "Application/GenericApplication.h"
#include "Window/GenericWindow.h"

namespace Durin::VulkanRHI
{
	auto FVulkanGenericPlatform::CreateSurface(void* InWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR
	{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		if (ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Surface))
		{
			DURIN_ERROR("Vulkan surface creation failed at the injected native boundary.");
			return VK_NULL_HANDLE;
		}
#endif
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
