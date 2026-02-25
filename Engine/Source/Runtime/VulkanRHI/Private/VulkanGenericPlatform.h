#pragma once

class FVulkanGenericPlatform
{
public:
	static auto CreateSurface(void* InWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR;
};
