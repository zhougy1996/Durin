#pragma once

#include "PixelFormat.h"
#include "RHIDefinitions.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFence;
	class FVulkanPayload;

	auto ConvertToVulkanFormat(EPixelFormat InFormat) -> vk::Format;
	auto ConvertToVulkanFormat(EVertexElementType InType) -> vk::Format;
	auto ConvertToVulkanBufferUsageFlags(EBufferUsageFlags InUsage) -> vk::BufferUsageFlags;

	auto GetFormatElementSize(vk::Format InFormat) -> uint32;

	extern std::atomic<uint64> GVulkanBufferHandleIdCounter;
	extern std::atomic<uint64> GVulkanBufferViewHandleIdCounter;
	extern std::atomic<uint64> GVulkanImageViewHandleIdCounter;
	extern std::atomic<uint64> GVulkanSamplerHandleIdCounter;
	extern std::atomic<uint64> GVulkanDSetLayoutHandleIdCounter;

} // namespace Doge::VulkanRHI