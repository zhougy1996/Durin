#include "VulkanPresentationSupport.h"

namespace Durin::VulkanRHI
{
	auto QueryNativeVulkanPresentationSupport(
		vk::PhysicalDevice Gpu,
		uint32 QueueFamilyIndex,
		vk::SurfaceKHR PresentationSurface) -> bool
	{
		if (!PresentationSurface) return false;
		return Gpu.getSurfaceSupportKHR(
			QueueFamilyIndex, PresentationSurface) == vk::True;
	}
}
