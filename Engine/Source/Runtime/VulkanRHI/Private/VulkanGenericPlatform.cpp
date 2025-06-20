#include "VulkanGenericPlatform.h"

auto FVulkanGenericPlatform::CreateSurface(void* WindowHandle, vk::Instance Instance) -> vk::SurfaceKHR
{
	vk::Win32SurfaceCreateInfoKHR createInfo;
	createInfo.hwnd = (HWND)(WindowHandle);
	createInfo.hinstance = GetModuleHandle(nullptr);
	vk::SurfaceKHR Surface = Instance.createWin32SurfaceKHR(createInfo);

	return Surface;
}