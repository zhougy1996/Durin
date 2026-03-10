#include "VulkanGenericPlatform.h"

#include "Misc/ApplicationCoreGlobals.h"
#include "Application/GenericApplication.h"
#include "HAL/RunnableThread.h"
#include "Window/GenericWindow.h"

namespace Doge::VulkanRHI
{
	auto FVulkanGenericPlatform::CreateSurface(void* InWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR
	{
		check(IsInGameThread());
		auto Window = GApp->FindWindowByNativeWindowHandle(InWindowHandle);

		void* Surface = Window->CreateVulkanSurface(static_cast<void*>(Instance));
		VkSurfaceKHR RawSurface = static_cast<VkSurfaceKHR>(Surface);

		return vk::SurfaceKHR(RawSurface);
	}
}