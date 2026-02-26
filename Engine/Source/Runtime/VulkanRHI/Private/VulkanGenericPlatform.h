#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanGenericPlatform
	{
	public:
		static auto CreateSurface(void* InWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR;
	};
}
