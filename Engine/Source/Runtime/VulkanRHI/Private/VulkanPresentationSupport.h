#pragma once

#include "PCH.VulkanRHI.h"

namespace Durin::VulkanRHI
{
	// Queries presentation admission through the active platform adapter.
	auto QueryNativeVulkanPresentationSupport(
		vk::PhysicalDevice Gpu,
		uint32 QueueFamilyIndex,
		vk::SurfaceKHR PresentationSurface) -> bool;
}
