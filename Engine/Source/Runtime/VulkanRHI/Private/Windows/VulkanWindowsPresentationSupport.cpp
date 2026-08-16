#include "VulkanPresentationSupport.h"

namespace Durin::VulkanRHI
{
	auto QueryNativeVulkanPresentationSupport(
		vk::PhysicalDevice Gpu,
		uint32 QueueFamilyIndex,
		vk::SurfaceKHR) -> bool
	{
		return Gpu.getWin32PresentationSupportKHR(QueueFamilyIndex);
	}
}
