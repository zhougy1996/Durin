#include "VulkanPresentationSupport.h"

namespace Durin::VulkanRHI
{
	auto QueryNativeVulkanPresentationSupport(
		vk::PhysicalDevice Gpu,
		uint32 QueueFamilyIndex) -> bool
	{
		return Gpu.getWin32PresentationSupportKHR(QueueFamilyIndex);
	}
}
