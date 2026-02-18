#include "VulkanGenericPlatform.h"

#include "ThirdParty/Glfw/GlfwCommon.h"

auto FVulkanGenericPlatform::CreateSurface(void* WindowHandle, vk::Instance Instance) -> vk::SurfaceKHR
{
#if defined _Win32
	vk::Win32SurfaceCreateInfoKHR createInfo;
	createInfo.hwnd = (HWND)(WindowHandle);
	createInfo.hinstance = GetModuleHandle(nullptr);
	vk::SurfaceKHR Surface = Instance.createWin32SurfaceKHR(createInfo);
#elif defined(__APPLE__)
	vk::MetalSurfaceCreateInfoEXT createInfo;
	createInfo.pLayer = static_cast<const CAMetalLayer*>(WindowHandle);

	vk::SurfaceKHR Surface = Instance.createMetalSurfaceEXT(createInfo);
#endif

	return Surface;
}