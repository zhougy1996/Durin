#pragma once

#include "CoreMinimal.h"

#include "RHI.h"
#include "RHIThread.h"
#include "Backend/RHICreation.h"

#include "VulkanPlatform.h"

#include "PixelFormat.h"
#include "RHIDefinitions.h"
#include "Logging/Logger.h"
#include "Threading/RunnableThread.h"

namespace Durin::VulkanRHI
{
	[[noreturn]] inline auto TerminateVulkanFailure(vk::Result Result, const char* Operation) -> void
	{
		RHITerminateOnFailure(Operation, Result == vk::Result::eErrorOutOfDeviceMemory || Result == vk::Result::eErrorOutOfHostMemory ? "out of memory" : "Vulkan operation failed", static_cast<int64>(Result));
	}


	class FVulkanDevice;
	class FVulkanFence;
	class FVulkanPayload;

	auto ToVulkan_Extent3D(const FIntVector& Size) -> vk::Extent3D;
	auto ToVulkan_TextureDimension(ETextureDimension Dimension) -> vk::ImageViewType;

	auto ToVulkan_PixelFormat(EPixelFormat InFormat) -> vk::Format;
	auto ToVulkan_VertexElementType(EVertexElementType InType) -> vk::Format;
	auto ToVulkan_BufferUsageFlags(EBufferUsageFlags InUsage) -> vk::BufferUsageFlags;
	auto ToVulkan_ShaderStageFlags(EShaderStageFlags InFlags) -> vk::ShaderStageFlags;
	auto ToVulkan_RHIBindingType(ERHIBindingType InType) -> vk::DescriptorType;
	auto ToVulkan_SamplerFilter(ESamplerFilter InFilter) -> vk::Filter;
	auto ToVulkan_SamplerMipmapMode(ESamplerMipmapMode InMode) -> vk::SamplerMipmapMode;
	auto ToVulkan_SamplerAddressMode(ESamplerAddressMode InMode) -> vk::SamplerAddressMode;
	auto ToVulkan_SamplerCompareOp(ESamplerCompareOp InCompareOp) -> vk::CompareOp;
	auto ToVulkan_SamplerBorderColor(ESamplerBorderColor InBorderColor) -> vk::BorderColor;

	extern std::atomic<uint64> GVulkanBufferHandleIdCounter;
	extern std::atomic<uint64> GVulkanBufferViewHandleIdCounter;
	extern std::atomic<uint64> GVulkanImageViewHandleIdCounter;
	extern std::atomic<uint64> GVulkanSamplerHandleIdCounter;

} // namespace Durin::VulkanRHI
