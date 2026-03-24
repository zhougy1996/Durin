#pragma once

#include "RHI.h"
#include "VulkanDynamicRHI.h"

namespace Doge::VulkanRHI
{
	auto ConvertToVulkanFormat(EPixelFormat InFormat) -> vk::Format;

	auto ConvertToVulkanBufferUsageFlags(EBufferUsageFlags InUsage) -> vk::BufferUsageFlags;

} // namespace Doge::VulkanRHI