#pragma once

#include "CoreMinimal.h"

#include "RHI.h"

#include "VulkanPlatform.h"

#include "PixelFormat.h"
#include "RHIDefinitions.h"
#include "Threading/RunnableThread.h"

namespace Durin::VulkanRHI
{
	// Unknown results, device loss, and validation failures are never recoverable.
	inline auto IsRecoverableVulkanCreationError(vk::Result Result) -> bool
	{
		switch (Result)
		{
		case vk::Result::eErrorOutOfHostMemory:
		case vk::Result::eErrorOutOfDeviceMemory:
		case vk::Result::eErrorFormatNotSupported:
		case vk::Result::eErrorTooManyObjects:
		case vk::Result::eErrorFragmentedPool:
		case vk::Result::eErrorOutOfPoolMemory:
			return true;
		default:
			return false;
		}
	}

	class FVulkanDevice;
	class FVulkanFence;
	class FVulkanPayload;

	// Inline mode intentionally preserves the pre-threaded diagnostic path.
	// Once a dedicated owner exists, every backend mutation must observe it.
	inline auto CheckVulkanRHIThread() -> void
	{
		if (GRHIThread)
		{
			CheckRHIThread();
		}
	}

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
