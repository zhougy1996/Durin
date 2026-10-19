#pragma once

#include "CoreMinimal.h"

#include "RHI.h"
#include "RHIThread.h"

#include "VulkanPlatform.h"

#include "PixelFormat.h"
#include "RHIDefinitions.h"
#include "Logging/Logger.h"
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

	[[noreturn]] inline auto ThrowVulkanError(vk::Result Result, std::string_view Operation) -> void
	{
		if (Result == vk::Result::eErrorDeviceLost) throw vk::DeviceLostError(std::string(Operation));
		throw vk::SystemError(vk::make_error_code(Result), std::string(Operation));
	}

	// Diagnostics only; each creation path decides whether to return null or throw.
	inline auto LogVulkanCreationFailure(vk::Result Result, std::string_view Kind,
		std::string_view DebugName = {}) -> void
	{
		DURIN_ERROR("Failed to create Vulkan {} '{}': {} (native code={})", Kind,
			DebugName.empty() ? "<unnamed>" : DebugName, vk::to_string(Result), static_cast<int32>(Result));
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
