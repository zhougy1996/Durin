#include "VulkanRHIPrivate.h"

#include "RHICommandList.h"
#include "Vulkan/utility/vk_format_utils.h"

#include "VulkanDevice.h"
#include "VulkanBuffer.h"
#include "VulkanDescriptorSets.h"
#include "VulkanSubmission.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCompletion.h"
#include "VulkanContext.h"
#include "VulkanDynamicRHI.h"

namespace Durin::VulkanRHI
{
	auto ExecuteFallibleVulkanCreationOperation(
		std::function<void()> Operation,
		size_t OwnedPayloadBytes) -> FRHIFallibleOperationResult
	{
		if (!GRHIThread || !IsInRHIThread())
		{
			return GCommandListExecutor.ExecuteFallibleSynchronousOperation(
				false, std::move(Operation), OwnedPayloadBytes);
		}

		FRHIFallibleOperationResult Result;
		try
		{
			Operation();
		}
		catch (const std::exception& Exception)
		{
			Result.bSucceeded = false;
			Result.Diagnostic = Exception.what();
		}
		catch (...)
		{
			Result.bSucceeded = false;
			Result.Diagnostic =
				"Fallible Vulkan creation failed with an unknown exception.";
		}
		return Result;
	}

#if DURIN_VULKAN_TEST_FAILURE_INJECTION
	std::atomic<uint64> GVulkanRenderPassEntryCount = 0;
	std::atomic<uint64> GVulkanFramebufferEntryCount = 0;
	std::atomic<uint64> GVulkanDescriptorSetLayoutEntryCount = 0;
	std::atomic<uint64> GVulkanPipelineLayoutEntryCount = 0;
	std::atomic<uint64> GVulkanCreatedFramebufferViewCount = 0;
	std::atomic<uint64> GVulkanReleasedFramebufferViewCount = 0;
	std::atomic<uint64> GVulkanCreatedFramebufferCount = 0;
	std::atomic<uint64> GVulkanReleasedFramebufferCount = 0;
	std::atomic<uint64> GVulkanSync2BufferBarrierCount = 0;
	std::atomic<uint64> GVulkanLegacyBufferBarrierCount = 0;
	std::atomic<uint64> GVulkanSync2ImageBarrierCount = 0;
	std::atomic<uint64> GVulkanLegacyImageBarrierCount = 0;
	std::atomic<int32> GVulkanBarrierPathOverride = -1;
	std::atomic<uint64> GVulkanBindingValidationVisitCount = 0;
	std::atomic<uint64> GVulkanDescriptorOccupancyVerificationVisitCount = 0;
	std::atomic<uint64> GVulkanDescriptorOccupancyMutationCount = 0;
	std::atomic<uint64> GVulkanDebugMessengerCreatedCount = 0;
	std::atomic<uint64> GVulkanDebugMessengerDestroyedCount = 0;
	std::atomic<uint64> GVulkanDebugMessengerActiveCount = 0;
	std::atomic<uint64> GVulkanDebugLifecycleSequence = 0;
	std::atomic<uint64> GVulkanLastMessengerDestroySequence = 0;
	std::atomic<uint64> GVulkanLastInstanceDestroySequence = 0;
	std::mutex GVulkanDebugUtilsTestEventMutex;
	std::vector<FVulkanDebugUtilsTestEvent> GVulkanDebugUtilsTestEvents;

	auto ResetVulkanDebugUtilsEventsForTest() -> void
	{
		std::lock_guard Lock(GVulkanDebugUtilsTestEventMutex);
		GVulkanDebugUtilsTestEvents.clear();
	}

	auto RecordVulkanDebugUtilsEventForTest(
		EVulkanDebugUtilsTestEventType Type, vk::ObjectType ObjectType,
		std::string_view Name) -> void
	{
		std::lock_guard Lock(GVulkanDebugUtilsTestEventMutex);
		GVulkanDebugUtilsTestEvents.push_back({Type, ObjectType, std::string(Name)});
	}

	auto GetVulkanDebugUtilsEventsForTest()
		-> std::vector<FVulkanDebugUtilsTestEvent>
	{
		std::lock_guard Lock(GVulkanDebugUtilsTestEventMutex);
		return GVulkanDebugUtilsTestEvents;
	}

	auto ResetVulkanDebugMessengerTestStats() -> void
	{
		check(GVulkanDebugMessengerActiveCount.load(std::memory_order_acquire) == 0);
		GVulkanDebugMessengerCreatedCount.store(0, std::memory_order_release);
		GVulkanDebugMessengerDestroyedCount.store(0, std::memory_order_release);
		GVulkanDebugLifecycleSequence.store(0, std::memory_order_release);
		GVulkanLastMessengerDestroySequence.store(0, std::memory_order_release);
		GVulkanLastInstanceDestroySequence.store(0, std::memory_order_release);
	}

	auto RecordVulkanDebugMessengerCreatedForTest() -> void
	{
		GVulkanDebugMessengerCreatedCount.fetch_add(1, std::memory_order_relaxed);
		GVulkanDebugMessengerActiveCount.fetch_add(1, std::memory_order_relaxed);
	}

	auto RecordVulkanDebugMessengerDestroyedForTest() -> void
	{
		GVulkanDebugMessengerDestroyedCount.fetch_add(1, std::memory_order_relaxed);
		GVulkanDebugMessengerActiveCount.fetch_sub(1, std::memory_order_relaxed);
		GVulkanLastMessengerDestroySequence.store(
			GVulkanDebugLifecycleSequence.fetch_add(1, std::memory_order_relaxed) + 1,
			std::memory_order_release);
	}

	auto RecordVulkanInstanceDestroyedForTest() -> void
	{
		GVulkanLastInstanceDestroySequence.store(
			GVulkanDebugLifecycleSequence.fetch_add(1, std::memory_order_relaxed) + 1,
			std::memory_order_release);
	}

	auto GetVulkanDebugMessengerTestStats()
		-> FVulkanDebugMessengerTestStats
	{
		return {
			.CreatedCount = GVulkanDebugMessengerCreatedCount.load(std::memory_order_acquire),
			.DestroyedCount = GVulkanDebugMessengerDestroyedCount.load(std::memory_order_acquire),
			.ActiveCount = GVulkanDebugMessengerActiveCount.load(std::memory_order_acquire),
			.LastMessengerDestroySequence = GVulkanLastMessengerDestroySequence.load(std::memory_order_acquire),
			.LastInstanceDestroySequence = GVulkanLastInstanceDestroySequence.load(std::memory_order_acquire),
		};
	}

	auto GetVulkanStructuralCacheTestStats() -> FVulkanStructuralCacheTestStats
	{
		return {
			.RenderPassEntryCount = GVulkanRenderPassEntryCount.load(std::memory_order_acquire),
			.FramebufferEntryCount = GVulkanFramebufferEntryCount.load(std::memory_order_acquire),
			.DescriptorSetLayoutEntryCount = GVulkanDescriptorSetLayoutEntryCount.load(std::memory_order_acquire),
			.PipelineLayoutEntryCount = GVulkanPipelineLayoutEntryCount.load(std::memory_order_acquire),
			.CreatedFramebufferViewCount = GVulkanCreatedFramebufferViewCount.load(std::memory_order_acquire),
			.ReleasedFramebufferViewCount = GVulkanReleasedFramebufferViewCount.load(std::memory_order_acquire),
			.CreatedFramebufferCount = GVulkanCreatedFramebufferCount.load(std::memory_order_acquire),
			.ReleasedFramebufferCount = GVulkanReleasedFramebufferCount.load(std::memory_order_acquire),
		};
	}

	namespace
	{
		std::array<std::atomic<bool>, static_cast<size_t>(EVulkanCreateFailurePoint::Count)>
			GArmedVulkanCreateFailures{};
	}

	auto ArmVulkanCreateFailure(EVulkanCreateFailurePoint FailurePoint) -> void
	{
		GArmedVulkanCreateFailures[static_cast<size_t>(FailurePoint)].store(true, std::memory_order_release);
	}

	auto ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint FailurePoint) -> bool
	{
		return GArmedVulkanCreateFailures[static_cast<size_t>(FailurePoint)].exchange(
			false, std::memory_order_acq_rel);
	}

	auto ResetVulkanCreateFailures() -> void
	{
		for (std::atomic<bool>& Failure : GArmedVulkanCreateFailures)
		{
			Failure.store(false, std::memory_order_release);
		}
	}

	auto ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint FailurePoint) -> void
	{
		if (ConsumeVulkanCreateFailure(FailurePoint))
		{
			throw vk::SystemError(
				vk::make_error_code(vk::Result::eErrorOutOfDeviceMemory),
				"Injected Vulkan native creation failure");
		}
	}

	auto GetVulkanCompletionTestStats() -> FVulkanCompletionTestStats
	{
		CheckVulkanRHIThread();
		FVulkanDevice* Device = FVulkanDynamicRHI::Get().GetDeviceForTesting();
		if (!Device)
		{
			return {};
		}
		FVulkanCompletionTracker& Tracker = Device->GetCompletionTracker();
		return {
			.LastReservedToken = Tracker.GetLastReservedToken(),
			.LastSubmittedToken = Tracker.GetLastSubmittedToken(),
			.CompletedToken = Tracker.GetCompletedToken(),
			.PendingSubmissionCount = Tracker.GetPendingSubmissionCount()};
	}

	auto GetVulkanBackendPoolTestStats() -> FVulkanBackendPoolTestStats
	{
		CheckVulkanRHIThread();
		FVulkanDevice* Device = FVulkanDynamicRHI::Get().GetDeviceForTesting();
		if (!Device)
		{
			return {};
		}
		return {
			.DynamicUniformTokens = Device->GetDynamicUniformBufferAllocator()
				.GetProducerTokensForTesting(),
			.DescriptorPoolTokens = Device->GetGlobalDescriptorPool()
				.GetBatchTokensForTesting()};
	}

	auto SubmitAndRetireDescriptorPoolsForTesting() -> uint64
	{
		CheckVulkanRHIThread();
		FVulkanDevice* Device = FVulkanDynamicRHI::Get().GetDeviceForTesting();
		const FVulkanCompletionToken Token =
			Device->GetImmediateContext()->Finalize();
		Device->GetGlobalDescriptorPool().RetireUsedPools(Token);
		return Token;
	}

	auto WaitForAllVulkanSubmissionsForTesting() -> void
	{
		CheckVulkanRHIThread();
		FVulkanDynamicRHI::Get().GetDeviceForTesting()
			->GetCompletionTracker().WaitForAll();
	}

	auto ReleaseCompletedVulkanResourcesForTesting() -> void
	{
		CheckVulkanRHIThread();
		FVulkanDynamicRHI::Get().GetDeviceForTesting()
			->GetDeferredDeletionQueue().ReleaseResources();
	}

	auto ResetVulkanHotPathWorkTestStats() -> void
	{
		GVulkanSync2BufferBarrierCount.store(0, std::memory_order_release);
		GVulkanLegacyBufferBarrierCount.store(0, std::memory_order_release);
		GVulkanSync2ImageBarrierCount.store(0, std::memory_order_release);
		GVulkanLegacyImageBarrierCount.store(0, std::memory_order_release);
		GVulkanBindingValidationVisitCount.store(0, std::memory_order_release);
		GVulkanDescriptorOccupancyVerificationVisitCount.store(0, std::memory_order_release);
		GVulkanDescriptorOccupancyMutationCount.store(0, std::memory_order_release);
	}

	auto GetVulkanHotPathWorkTestStats() -> FVulkanHotPathWorkTestStats
	{
		return {
			.Sync2BufferBarriers = GVulkanSync2BufferBarrierCount.load(std::memory_order_acquire),
			.LegacyBufferBarriers = GVulkanLegacyBufferBarrierCount.load(std::memory_order_acquire),
			.Sync2ImageBarriers = GVulkanSync2ImageBarrierCount.load(std::memory_order_acquire),
			.LegacyImageBarriers = GVulkanLegacyImageBarrierCount.load(std::memory_order_acquire),
			.BindingValidationVisits = GVulkanBindingValidationVisitCount.load(std::memory_order_acquire),
			.DescriptorOccupancyVerificationVisits = GVulkanDescriptorOccupancyVerificationVisitCount.load(std::memory_order_acquire),
			.DescriptorOccupancyMutations = GVulkanDescriptorOccupancyMutationCount.load(std::memory_order_acquire)};
	}

	auto SetVulkanBarrierPathOverrideForTest(
		std::optional<bool> bUseSynchronization2) -> void
	{
		GVulkanBarrierPathOverride.store(
			bUseSynchronization2.has_value() ? (*bUseSynchronization2 ? 1 : 0) : -1,
			std::memory_order_release);
	}

	auto SelectVulkanSynchronization2BarrierPath(
		bool bSupportsSynchronization2) -> bool
	{
		const int32 Override = GVulkanBarrierPathOverride.load(
			std::memory_order_acquire);
		checkf(Override != 1 || bSupportsSynchronization2,
			"Synchronization2 barrier override requires device support.");
		return Override < 0 ? bSupportsSynchronization2 : Override != 0;
	}
#endif

	struct FVulkanFormatMapping
	{
		EPixelFormat RhiFormat;
		vk::Format VulkanFormat;
	};

	static constexpr std::array<FVulkanFormatMapping, static_cast<size_t>(EPixelFormat::Count)> VulkanFormatMap = {{
		{EPixelFormat::Unknown, vk::Format::eUndefined},
		{EPixelFormat::R8_UINT, vk::Format::eR8Uint},
		{EPixelFormat::R8_SINT, vk::Format::eR8Sint},
		{EPixelFormat::R8_UNORM, vk::Format::eR8Unorm},
		{EPixelFormat::R8_SNORM, vk::Format::eR8Snorm},
		{EPixelFormat::RG8_UINT, vk::Format::eR8G8Uint},
		{EPixelFormat::RG8_SINT, vk::Format::eR8G8Sint},
		{EPixelFormat::RG8_UNORM, vk::Format::eR8G8Unorm},
		{EPixelFormat::RG8_SNORM, vk::Format::eR8G8Snorm},
		{EPixelFormat::R16_UINT, vk::Format::eR16Uint},
		{EPixelFormat::R16_SINT, vk::Format::eR16Sint},
		{EPixelFormat::R16_UNORM, vk::Format::eR16Unorm},
		{EPixelFormat::R16_SNORM, vk::Format::eR16Snorm},
		{EPixelFormat::R16_FLOAT, vk::Format::eR16Sfloat},
		{EPixelFormat::BGRA4_UNORM, vk::Format::eA4R4G4B4UnormPack16}, // this format matches the bit layout of DXGI_FORMAT_B4G4R4A4_UNORM
		{EPixelFormat::B5G6R5_UNORM, vk::Format::eB5G6R5UnormPack16},
		{EPixelFormat::B5G5R5A1_UNORM, vk::Format::eB5G5R5A1UnormPack16},
		{EPixelFormat::RGBA8_UINT, vk::Format::eR8G8B8A8Uint},
		{EPixelFormat::RGBA8_SINT, vk::Format::eR8G8B8A8Sint},
		{EPixelFormat::RGBA8_UNORM, vk::Format::eR8G8B8A8Unorm},
		{EPixelFormat::RGBA8_SNORM, vk::Format::eR8G8B8A8Snorm},
		{EPixelFormat::BGRA8_UNORM, vk::Format::eB8G8R8A8Unorm},
		{EPixelFormat::BGRX8_UNORM, vk::Format::eUndefined}, // Not supported on Vulkan
		{EPixelFormat::SRGBA8_UNORM, vk::Format::eR8G8B8A8Srgb},
		{EPixelFormat::SBGRA8_UNORM, vk::Format::eB8G8R8A8Srgb},
		{EPixelFormat::SBGRX8_UNORM, vk::Format::eUndefined}, // Not supported on Vulkan
		{EPixelFormat::R10G10B10A2_UNORM, vk::Format::eA2B10G10R10UnormPack32},
		{EPixelFormat::R11G11B10_FLOAT, vk::Format::eB10G11R11UfloatPack32},
		{EPixelFormat::RG16_UINT, vk::Format::eR16G16Uint},
		{EPixelFormat::RG16_SINT, vk::Format::eR16G16Sint},
		{EPixelFormat::RG16_UNORM, vk::Format::eR16G16Unorm},
		{EPixelFormat::RG16_SNORM, vk::Format::eR16G16Snorm},
		{EPixelFormat::RG16_FLOAT, vk::Format::eR16G16Sfloat},
		{EPixelFormat::R32_UINT, vk::Format::eR32Uint},
		{EPixelFormat::R32_SINT, vk::Format::eR32Sint},
		{EPixelFormat::R32_FLOAT, vk::Format::eR32Sfloat},
		{EPixelFormat::RGBA16_UINT, vk::Format::eR16G16B16A16Uint},
		{EPixelFormat::RGBA16_SINT, vk::Format::eR16G16B16A16Sint},
		{EPixelFormat::RGBA16_FLOAT, vk::Format::eR16G16B16A16Sfloat},
		{EPixelFormat::RGBA16_UNORM, vk::Format::eR16G16B16A16Unorm},
		{EPixelFormat::RGBA16_SNORM, vk::Format::eR16G16B16A16Snorm},
		{EPixelFormat::RG32_UINT, vk::Format::eR32G32Uint},
		{EPixelFormat::RG32_SINT, vk::Format::eR32G32Sint},
		{EPixelFormat::RG32_FLOAT, vk::Format::eR32G32Sfloat},
		{EPixelFormat::RGB32_UINT, vk::Format::eR32G32B32Uint},
		{EPixelFormat::RGB32_SINT, vk::Format::eR32G32B32Sint},
		{EPixelFormat::RGB32_FLOAT, vk::Format::eR32G32B32Sfloat},
		{EPixelFormat::RGBA32_UINT, vk::Format::eR32G32B32A32Uint},
		{EPixelFormat::RGBA32_SINT, vk::Format::eR32G32B32A32Sint},
		{EPixelFormat::RGBA32_FLOAT, vk::Format::eR32G32B32A32Sfloat},
		{EPixelFormat::D16, vk::Format::eD16Unorm},
		{EPixelFormat::D24S8, vk::Format::eD24UnormS8Uint},
		{EPixelFormat::X24G8_UINT, vk::Format::eD24UnormS8Uint},
		{EPixelFormat::D32, vk::Format::eD32Sfloat},
		{EPixelFormat::D32S8, vk::Format::eD32SfloatS8Uint},
		{EPixelFormat::X32G8_UINT, vk::Format::eD32SfloatS8Uint},
		{EPixelFormat::BC1_UNORM, vk::Format::eBc1RgbaUnormBlock},
		{EPixelFormat::BC1_UNORM_SRGB, vk::Format::eBc1RgbaSrgbBlock},
		{EPixelFormat::BC2_UNORM, vk::Format::eBc2UnormBlock},
		{EPixelFormat::BC2_UNORM_SRGB, vk::Format::eBc2SrgbBlock},
		{EPixelFormat::BC3_UNORM, vk::Format::eBc3UnormBlock},
		{EPixelFormat::BC3_UNORM_SRGB, vk::Format::eBc3SrgbBlock},
		{EPixelFormat::BC4_UNORM, vk::Format::eBc4UnormBlock},
		{EPixelFormat::BC4_SNORM, vk::Format::eBc4SnormBlock},
		{EPixelFormat::BC5_UNORM, vk::Format::eBc5UnormBlock},
		{EPixelFormat::BC5_SNORM, vk::Format::eBc5SnormBlock},
		{EPixelFormat::BC6H_UFLOAT, vk::Format::eBc6HUfloatBlock},
		{EPixelFormat::BC6H_SFLOAT, vk::Format::eBc6HSfloatBlock},
		{EPixelFormat::BC7_UNORM, vk::Format::eBc7UnormBlock},
		{EPixelFormat::BC7_UNORM_SRGB, vk::Format::eBc7SrgbBlock},
	}};

	auto ToVulkan_Extent3D(const FIntVector& Size) -> vk::Extent3D
	{
		return vk::Extent3D{static_cast<uint32>(Size.x), static_cast<uint32>(Size.y), static_cast<uint32>(Size.z)};
	}

	auto ToVulkan_TextureDimension(ETextureDimension Dimension) -> vk::ImageViewType
	{
		switch (Dimension)
		{
		case ETextureDimension::Texture2D:
			return vk::ImageViewType::e2D;
		case ETextureDimension::Texture3D:
			return vk::ImageViewType::e3D;
		case ETextureDimension::TextureCube:
			return vk::ImageViewType::eCube;
		case ETextureDimension::Texture2DArray:
			return vk::ImageViewType::e2DArray;
		case ETextureDimension::TextureCubeArray:
			return vk::ImageViewType::eCubeArray;
		default:
			DURIN_ERROR("Unsupported Vulkan texture dimension: value={}.", static_cast<int32>(Dimension));
			return vk::ImageViewType::e2D;
		}
	}

	auto ToVulkan_PixelFormat(EPixelFormat InFormat) -> vk::Format
	{
		check(InFormat < EPixelFormat::Count);
		check(VulkanFormatMap[static_cast<uint32>(InFormat)].RhiFormat == InFormat);
		return VulkanFormatMap[static_cast<uint32>(InFormat)].VulkanFormat;
	}

	auto ToVulkan_VertexElementType(EVertexElementType InType) -> vk::Format
	{
		switch (InType)
		{
		case EVertexElementType::Float1: return vk::Format::eR32Sfloat;
		case EVertexElementType::Float2: return vk::Format::eR32G32Sfloat;
		case EVertexElementType::Float3: return vk::Format::eR32G32B32Sfloat;
		case EVertexElementType::Float4: return vk::Format::eR32G32B32A32Sfloat;
		case EVertexElementType::Color: return vk::Format::eR8G8B8A8Unorm;
		case EVertexElementType::UByte4N: return vk::Format::eR8G8B8A8Unorm;
		case EVertexElementType::Half2: return vk::Format::eR16G16Sfloat;
		case EVertexElementType::Half4: return vk::Format::eR16G16B16A16Sfloat;
		case EVertexElementType::Short2: return vk::Format::eR16G16Sint;
		case EVertexElementType::Short4: return vk::Format::eR16G16B16A16Sint;
		case EVertexElementType::Short2N: return vk::Format::eR16G16Snorm;
		case EVertexElementType::Short4N: return vk::Format::eR16G16B16A16Snorm;
		case EVertexElementType::UShort2: return vk::Format::eR16G16Uint;
		case EVertexElementType::UShort4: return vk::Format::eR16G16B16A16Uint;
		case EVertexElementType::UShort2N: return vk::Format::eR16G16Unorm;
		case EVertexElementType::UShort4N: return vk::Format::eR16G16B16A16Unorm;
		case EVertexElementType::UInt: return vk::Format::eR32Uint;
		case EVertexElementType::URGB10A2N: return vk::Format::eA2B10G10R10UnormPack32;
		default:
			DURIN_ERROR("Unsupported Vulkan vertex element type: value={}.", static_cast<int32>(InType));
			return vk::Format::eR32G32B32Sfloat;
		}
	}

	auto ToVulkan_BufferUsageFlags(EBufferUsageFlags InUsage) -> vk::BufferUsageFlags
	{
		vk::BufferUsageFlags UsageFlags{};

		if (EnumHasAnyFlags(InUsage, EBufferUsageFlags::Static | EBufferUsageFlags::Dynamic))
		{
			check(!EnumHasAllFlags(InUsage, EBufferUsageFlags::Static | EBufferUsageFlags::Dynamic)); // A buffer cannot be both static and dynamic
			UsageFlags |= vk::BufferUsageFlagBits::eTransferDst;
		}
		if (EnumHasAnyFlags(InUsage, EBufferUsageFlags::DestinationCopy))
		{
			UsageFlags |= vk::BufferUsageFlagBits::eTransferDst;
		}

		if (EnumHasAnyFlags(InUsage, EBufferUsageFlags::VertexBuffer))
		{
			UsageFlags |= vk::BufferUsageFlagBits::eVertexBuffer;
		}

		if (EnumHasAnyFlags(InUsage, EBufferUsageFlags::IndexBuffer))
		{
			UsageFlags |= vk::BufferUsageFlagBits::eIndexBuffer;
		}

		if (EnumHasAnyFlags(InUsage, EBufferUsageFlags::DrawIndirect))
		{
			UsageFlags |= vk::BufferUsageFlagBits::eIndirectBuffer;
		}

		if (EnumHasAnyFlags(InUsage, EBufferUsageFlags::UniformBuffer))
		{
			UsageFlags |= vk::BufferUsageFlagBits::eUniformBuffer;
		}

		if (EnumHasAnyFlags(InUsage, EBufferUsageFlags::SourceCopy))
		{
			UsageFlags |= vk::BufferUsageFlagBits::eTransferSrc;
		}
		if (EnumHasAnyFlags(InUsage, EBufferUsageFlags::FormattedBuffer))
		{
			UsageFlags |= vk::BufferUsageFlagBits::eUniformTexelBuffer
				| vk::BufferUsageFlagBits::eStorageTexelBuffer;
		}

		if (EnumHasAnyFlags(InUsage, EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ByteAddressBuffer | EBufferUsageFlags::ShaderResource))
		{
			UsageFlags |= vk::BufferUsageFlagBits::eStorageBuffer;
		}

		check(UsageFlags != vk::BufferUsageFlags{} || EnumHasAnyFlags(InUsage, EBufferUsageFlags::NullResource));
		return UsageFlags;
	}

	auto ToVulkan_ShaderStageFlags(EShaderStageFlags InFlags) -> vk::ShaderStageFlags
	{
		vk::ShaderStageFlags Result;
		if (EnumHasAnyFlags(InFlags, EShaderStageFlags::Vertex))
		{
			Result |= vk::ShaderStageFlagBits::eVertex;
		}
		if (EnumHasAnyFlags(InFlags, EShaderStageFlags::Fragment))
		{
			Result |= vk::ShaderStageFlagBits::eFragment;
		}
		if (EnumHasAnyFlags(InFlags, EShaderStageFlags::Geometry))
		{
			Result |= vk::ShaderStageFlagBits::eGeometry;
		}
		if (EnumHasAnyFlags(InFlags, EShaderStageFlags::Compute))
		{
			Result |= vk::ShaderStageFlagBits::eCompute;
		}
		return Result;
	}

	auto ToVulkan_RHIBindingType(ERHIBindingType InType) -> vk::DescriptorType
	{
		switch (InType)
		{
		case ERHIBindingType::UniformBuffer: return vk::DescriptorType::eUniformBuffer;
		case ERHIBindingType::UniformBufferDynamic: return vk::DescriptorType::eUniformBufferDynamic;
		case ERHIBindingType::Texture: return vk::DescriptorType::eSampledImage;
		case ERHIBindingType::Sampler: return vk::DescriptorType::eSampler;
		case ERHIBindingType::StorageBuffer: return vk::DescriptorType::eStorageBuffer;
		case ERHIBindingType::StorageImage: return vk::DescriptorType::eStorageImage;
		default:
			DURIN_ERROR("Unsupported Vulkan binding type: value={}.", static_cast<int32>(InType));
			return vk::DescriptorType::eUniformBuffer;
		}
	}

	auto ToVulkan_SamplerFilter(ESamplerFilter InFilter) -> vk::Filter
	{
		switch (InFilter)
		{
		case ESamplerFilter::Nearest:
			return vk::Filter::eNearest;
		case ESamplerFilter::Linear:
			return vk::Filter::eLinear;
		default:
			DURIN_ERROR("Unsupported Vulkan sampler filter: value={}.", static_cast<int32>(InFilter));
			return vk::Filter::eLinear;
		}
	}

	auto ToVulkan_SamplerMipmapMode(ESamplerMipmapMode InMode) -> vk::SamplerMipmapMode
	{
		switch (InMode)
		{
		case ESamplerMipmapMode::Nearest:
			return vk::SamplerMipmapMode::eNearest;
		case ESamplerMipmapMode::Linear:
			return vk::SamplerMipmapMode::eLinear;
		default:
			DURIN_ERROR("Unsupported Vulkan sampler mipmap mode: value={}.", static_cast<int32>(InMode));
			return vk::SamplerMipmapMode::eLinear;
		}
	}

	auto ToVulkan_SamplerAddressMode(ESamplerAddressMode InMode) -> vk::SamplerAddressMode
	{
		switch (InMode)
		{
		case ESamplerAddressMode::Repeat:
			return vk::SamplerAddressMode::eRepeat;
		case ESamplerAddressMode::MirroredRepeat:
			return vk::SamplerAddressMode::eMirroredRepeat;
		case ESamplerAddressMode::ClampToEdge:
			return vk::SamplerAddressMode::eClampToEdge;
		case ESamplerAddressMode::ClampToBorder:
			return vk::SamplerAddressMode::eClampToBorder;
		default:
			DURIN_ERROR("Unsupported Vulkan sampler address mode: value={}.", static_cast<int32>(InMode));
			return vk::SamplerAddressMode::eClampToEdge;
		}
	}

	auto ToVulkan_SamplerCompareOp(ESamplerCompareOp InCompareOp) -> vk::CompareOp
	{
		switch (InCompareOp)
		{
		case ESamplerCompareOp::Never:
			return vk::CompareOp::eNever;
		case ESamplerCompareOp::Less:
			return vk::CompareOp::eLess;
		case ESamplerCompareOp::Equal:
			return vk::CompareOp::eEqual;
		case ESamplerCompareOp::LessOrEqual:
			return vk::CompareOp::eLessOrEqual;
		case ESamplerCompareOp::Greater:
			return vk::CompareOp::eGreater;
		case ESamplerCompareOp::NotEqual:
			return vk::CompareOp::eNotEqual;
		case ESamplerCompareOp::GreaterOrEqual:
			return vk::CompareOp::eGreaterOrEqual;
		case ESamplerCompareOp::Always:
			return vk::CompareOp::eAlways;
		default:
			DURIN_ERROR("Unsupported Vulkan sampler compare operation: value={}.", static_cast<int32>(InCompareOp));
			return vk::CompareOp::eAlways;
		}
	}

	auto ToVulkan_SamplerBorderColor(ESamplerBorderColor InBorderColor) -> vk::BorderColor
	{
		switch (InBorderColor)
		{
		case ESamplerBorderColor::FloatTransparentBlack:
			return vk::BorderColor::eFloatTransparentBlack;
		case ESamplerBorderColor::IntTransparentBlack:
			return vk::BorderColor::eIntTransparentBlack;
		case ESamplerBorderColor::FloatOpaqueBlack:
			return vk::BorderColor::eFloatOpaqueBlack;
		case ESamplerBorderColor::IntOpaqueBlack:
			return vk::BorderColor::eIntOpaqueBlack;
		case ESamplerBorderColor::FloatOpaqueWhite:
			return vk::BorderColor::eFloatOpaqueWhite;
		case ESamplerBorderColor::IntOpaqueWhite:
			return vk::BorderColor::eIntOpaqueWhite;
		default:
			DURIN_ERROR("Unsupported Vulkan sampler border color: value={}.", static_cast<int32>(InBorderColor));
			return vk::BorderColor::eFloatTransparentBlack;
		}
	}

	std::atomic<uint64> GVulkanBufferHandleIdCounter = 0;
	std::atomic<uint64> GVulkanBufferViewHandleIdCounter = 0;
	std::atomic<uint64> GVulkanImageViewHandleIdCounter = 0;
	std::atomic<uint64> GVulkanSamplerHandleIdCounter = 0;
	std::atomic<uint64> GVulkanDSetLayoutHandleIdCounter = 0;
} // namespace Durin::VulkanRHI
