#pragma once

class FVulkanGenericPlatform
{
public:
	static auto CreateSurface(void* WindowHandle, vk::Instance Instance) -> vk::SurfaceKHR;
};
