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
		DebugMessenger,
		Surface,
		Device,
		Swapchain,
		SwapchainImageView,
		SwapchainSemaphore,
		SwapchainFence,
		Allocator,
		Image,
		ImageView,
		BufferView,
		RenderPass,
		FramebufferImageView,
		Framebuffer,
		DescriptorPool,
		DescriptorSetLayout,
		Buffer,
		ShaderModule,
		PipelineLayout,
		GraphicsPipeline,
		Sampler,
		VertexDeclaration,
		QueryPool,
		MappedMemoryFlush,
		MappedMemoryInvalidate,
		Count
	};

	VULKANRHI_API auto ArmVulkanCreateFailure(EVulkanCreateFailurePoint FailurePoint) -> void;
	VULKANRHI_API auto ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint FailurePoint) -> bool;
	VULKANRHI_API auto ResetVulkanCreateFailures() -> void;
	VULKANRHI_API auto ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint FailurePoint) -> void;

	struct FVulkanDebugMessengerTestStats
	{
		uint64 CreatedCount = 0;
		uint64 DestroyedCount = 0;
		uint64 ActiveCount = 0;
		uint64 LastMessengerDestroySequence = 0;
		uint64 LastInstanceDestroySequence = 0;
	};

	VULKANRHI_API auto ResetVulkanDebugMessengerTestStats() -> void;
	VULKANRHI_API auto RecordVulkanDebugMessengerCreatedForTest() -> void;
	VULKANRHI_API auto RecordVulkanDebugMessengerDestroyedForTest() -> void;
	VULKANRHI_API auto RecordVulkanInstanceDestroyedForTest() -> void;
	VULKANRHI_API auto GetVulkanDebugMessengerTestStats()
		-> FVulkanDebugMessengerTestStats;

	enum class EVulkanDebugUtilsTestEventType : uint8
	{
		ObjectName,
		LabelBegin,
		LabelEnd,
	};

	struct FVulkanDebugUtilsTestEvent
	{
		EVulkanDebugUtilsTestEventType Type =
			EVulkanDebugUtilsTestEventType::ObjectName;
		vk::ObjectType ObjectType = vk::ObjectType::eUnknown;
		std::string Name;
	};

	VULKANRHI_API auto ResetVulkanDebugUtilsEventsForTest() -> void;
	VULKANRHI_API auto RecordVulkanDebugUtilsEventForTest(
		EVulkanDebugUtilsTestEventType Type, vk::ObjectType ObjectType,
		std::string_view Name) -> void;
	VULKANRHI_API auto GetVulkanDebugUtilsEventsForTest()
		-> std::vector<FVulkanDebugUtilsTestEvent>;

	struct FVulkanGraphicsPipelineTestStats
	{
		uint64 CommittedPipelineCount = 0;
		uint64 DestroyedPipelineCount = 0;
		uint64 CreatedPipelineLayoutCount = 0;
		uint64 RolledBackPipelineLayoutCount = 0;
	};

	VULKANRHI_API auto GetVulkanGraphicsPipelineTestStats()
		-> FVulkanGraphicsPipelineTestStats;

	struct FVulkanStructuralCacheTestStats
	{
		uint64 RenderPassEntryCount = 0;
		uint64 FramebufferEntryCount = 0;
		uint64 DescriptorSetLayoutEntryCount = 0;
		uint64 PipelineLayoutEntryCount = 0;
		uint64 CreatedFramebufferViewCount = 0;
		uint64 ReleasedFramebufferViewCount = 0;
		uint64 CreatedFramebufferCount = 0;
		uint64 ReleasedFramebufferCount = 0;
	};

	extern std::atomic<uint64> GVulkanRenderPassEntryCount;
	extern std::atomic<uint64> GVulkanFramebufferEntryCount;
	extern std::atomic<uint64> GVulkanDescriptorSetLayoutEntryCount;
	extern std::atomic<uint64> GVulkanPipelineLayoutEntryCount;
	extern std::atomic<uint64> GVulkanCreatedFramebufferViewCount;
	extern std::atomic<uint64> GVulkanReleasedFramebufferViewCount;
	extern std::atomic<uint64> GVulkanCreatedFramebufferCount;
	extern std::atomic<uint64> GVulkanReleasedFramebufferCount;
	VULKANRHI_API auto GetVulkanStructuralCacheTestStats()
		-> FVulkanStructuralCacheTestStats;

	struct FVulkanCompletionTestStats
	{
		uint64 LastReservedToken = 0;
		uint64 LastSubmittedToken = 0;
		uint64 CompletedToken = 0;
		uint64 PendingSubmissionCount = 0;
	};

	VULKANRHI_API auto GetVulkanCompletionTestStats()
		-> FVulkanCompletionTestStats;
	struct FVulkanBackendPoolTestStats
	{
		std::array<uint64, kFrameInFlight> DynamicUniformTokens = {};
		std::array<uint64, kFrameInFlight> DescriptorPoolTokens = {};
	};
	VULKANRHI_API auto GetVulkanBackendPoolTestStats()
		-> FVulkanBackendPoolTestStats;
	VULKANRHI_API auto SubmitAndRetireDescriptorPoolsForTesting() -> uint64;
	VULKANRHI_API auto WaitForAllVulkanSubmissionsForTesting() -> void;
	VULKANRHI_API auto ReleaseCompletedVulkanResourcesForTesting() -> void;

	struct FVulkanHotPathWorkTestStats
	{
		uint64 Sync2BufferBarriers = 0;
		uint64 LegacyBufferBarriers = 0;
		uint64 Sync2ImageBarriers = 0;
		uint64 LegacyImageBarriers = 0;
		uint64 BindingValidationVisits = 0;
		uint64 DescriptorOccupancyVerificationVisits = 0;
		uint64 DescriptorOccupancyMutations = 0;
	};
	VULKANRHI_API auto ResetVulkanHotPathWorkTestStats() -> void;
	VULKANRHI_API auto GetVulkanHotPathWorkTestStats()
		-> FVulkanHotPathWorkTestStats;
	VULKANRHI_API auto SetVulkanBarrierPathOverrideForTest(
		std::optional<bool> bUseSynchronization2) -> void;
	VULKANRHI_API auto SelectVulkanSynchronization2BarrierPath(
		bool bSupportsSynchronization2) -> bool;
	extern std::atomic<uint64> GVulkanSync2BufferBarrierCount;
	extern std::atomic<uint64> GVulkanLegacyBufferBarrierCount;
	extern std::atomic<uint64> GVulkanSync2ImageBarrierCount;
	extern std::atomic<uint64> GVulkanLegacyImageBarrierCount;
	extern std::atomic<int32> GVulkanBarrierPathOverride;
	extern std::atomic<uint64> GVulkanBindingValidationVisitCount;
	extern std::atomic<uint64> GVulkanDescriptorOccupancyVerificationVisitCount;
	extern std::atomic<uint64> GVulkanDescriptorOccupancyMutationCount;
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
