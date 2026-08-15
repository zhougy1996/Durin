#include "VulkanPresentationSupport.h"

namespace Durin::VulkanRHI
{
	auto QueryNativeVulkanPresentationSupport(
		vk::PhysicalDevice Gpu,
		uint32 QueueFamilyIndex) -> bool
	{
		(void)Gpu;
		(void)QueueFamilyIndex;
		static std::atomic<bool> bReported = false;
		if (!bReported.exchange(true, std::memory_order_relaxed))
		{
			DURIN_ERROR(
				"macOS Vulkan presentation cannot be admitted before a Metal surface exists; "
				"surface-qualified MoltenVK device admission is deferred to macOS enablement M3.");
		}
		return false;
	}
}
