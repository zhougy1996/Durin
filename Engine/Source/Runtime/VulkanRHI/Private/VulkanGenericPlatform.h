#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanGenericPlatform
	{
	public:
		// Must be called from the main thread.
		static auto CreateSurface(void* InWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR;
	};
}
