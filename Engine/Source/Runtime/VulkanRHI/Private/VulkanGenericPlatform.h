#pragma once

namespace Durin::VulkanRHI
{
	// Supplies platform-specific Vulkan loader and presentation extensions.
	class FVulkanGenericPlatform
	{
	public:
		// Must be called from the main thread.
		static auto CreateSurface(void* InWindowHandle, vk::Instance Instance) -> vk::SurfaceKHR;
	};
}
