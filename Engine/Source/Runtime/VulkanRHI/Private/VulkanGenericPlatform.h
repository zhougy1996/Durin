#pragma once

class FVulkanGenericPlatform
{
public:
	static auto CreateSurface(void* GlfwWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR;
};
