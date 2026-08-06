#pragma once

#include "PixelFormat.h"
#include "RHIDefinitions.h"
#include "Threading/RunnableThread.h"
#include "VulkanRHIAPI.h"

namespace Durin
{
	struct FRHIFallibleOperationResult;
}

namespace Durin::VulkanRHI
{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
	// Test-only Vulkan factory/native/VMA boundaries. Each armed point fails exactly once.
	enum class EVulkanCreateFailurePoint : uint8
	{
		Instance,
		Device,
		Swapchain,
		SwapchainImageView,
		SwapchainSemaphore,
		SwapchainFence,
		Allocator,
		Image,
		ImageView,
		Buffer,
		ShaderModule,
		GraphicsPipeline,
		Sampler,
		VertexDeclaration,
		Count
	};

	VULKANRHI_API auto ArmVulkanCreateFailure(EVulkanCreateFailurePoint FailurePoint) -> void;
	VULKANRHI_API auto ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint FailurePoint) -> bool;
	VULKANRHI_API auto ResetVulkanCreateFailures() -> void;
	VULKANRHI_API auto ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint FailurePoint) -> void;
#endif

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

	// Uses the executor only when creation crosses to the RHI thread. Factories
	// already running on that owner catch locally to avoid self-enqueue/wait.
	auto ExecuteFallibleVulkanCreationOperation(
		std::function<void()> Operation,
		size_t OwnedPayloadBytes = 0) -> FRHIFallibleOperationResult;

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
	extern std::atomic<uint64> GVulkanDSetLayoutHandleIdCounter;

} // namespace Durin::VulkanRHI
