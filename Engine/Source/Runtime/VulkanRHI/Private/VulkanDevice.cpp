#include "VulkanDevice.h"

#include "RHICommandList.h"
#include "VulkanDynamicRHI.h"
#include "VulkanExtensions.h"
#include "VulkanContext.h"
#include "VulkanRenderPass.h"
#include "VulkanPipeline.h"
#include "VulkanQueue.h"
#include "VulkanSubmission.h"
#include "VulkanDescriptorSets.h"
#include "VulkanBuffer.h"

namespace Durin::VulkanRHI
{
	uint64 GVulkanRHIDeletionFrameNumber = 0;

	constexpr uint64 GVulkanNumFramesToWaitForResourceDelete = kFrameInFlight;

	FDeferredDeletionQueue::FDeferredDeletionQueue(FVulkanDevice* InDevice)
		: Device(InDevice)
	{
	}

	auto FDeferredDeletionQueue::ReleaseResources(bool bDeleteImmediately) -> void
	{
		if (bDeleteImmediately)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			ReleaseResourceImmediately(Entries);
			Entries.clear();
		}
		else
		{
			std::vector<FEntry> EntriesToDelete;
			{
				std::lock_guard<std::mutex> Lock(Mutex);

				auto DeleteSubRange = std::ranges::partition(Entries, [&](const FEntry& Entry) {
					// Return true for elements we want to KEEP this Frame
					return GVulkanRHIDeletionFrameNumber <= Entry.FrameNumber + GVulkanNumFramesToWaitForResourceDelete;
				});

				if (DeleteSubRange.begin() != Entries.end())
				{
					EntriesToDelete.insert(EntriesToDelete.end(), std::make_move_iterator(DeleteSubRange.begin()), std::make_move_iterator(DeleteSubRange.end()));
					Entries.erase(DeleteSubRange.begin(), DeleteSubRange.end());
				}
			}

			if (!EntriesToDelete.empty())
			{
				ReleaseResourceImmediately(EntriesToDelete);
			}
		}
	}

	auto FDeferredDeletionQueue::Clear() -> void
	{
		ReleaseResources(true);
	}

	auto FDeferredDeletionQueue::EnqueueGenericResource(EType Type, uint64 Handle) -> void
	{
		Entries.emplace_back(Type, GVulkanRHIDeletionFrameNumber, Handle);
	}

	auto FDeferredDeletionQueue::EnqueueAllocatedResource(EType Type, uint64 Handle, const FVulkanAllocation& Allocation) -> void
	{
		Entries.emplace_back(Type, GVulkanRHIDeletionFrameNumber, Handle, Allocation); // Copy allocation here
	}

#define DURIN_VK_DESTROY_CASE(Type, ...)                                                 \
	case EType::Type:                                                                   \
		__VA_ARGS__                                                                     \
		DeviceHandle.destroy##Type(vk::Type{reinterpret_cast<Vk##Type>(Entry.Handle)}); \
		break

#define DURIN_VMA_DESTROY_CASE(Type, ...)                                                                   \
	case EType::Type:                                                                                      \
		__VA_ARGS__                                                                                        \
		MemoryManager.Destroy##Type(Entry.Allocation, vk::Type{reinterpret_cast<Vk##Type>(Entry.Handle)}); \
		break

	auto FDeferredDeletionQueue::ReleaseResourceImmediately(std::vector<FEntry>& InEntries) const -> void
	{
		const vk::Device DeviceHandle = Device->GetHandle();
		FVulkanMemoryManager& MemoryManager = Device->GetMemoryManager();

		for (FEntry& Entry : InEntries)
		{
			if (Entry.Allocation.IsValid())
			{
				switch (Entry.Type)
				{
					DURIN_VMA_DESTROY_CASE(Image);
					DURIN_VMA_DESTROY_CASE(Buffer);
				default:
					DURIN_ERROR("Unknown Vulkan resource type {} for vma", static_cast<uint32>(Entry.Type));
					break;
				}
			}
			else
			{
				switch (Entry.Type)
				{
					DURIN_VK_DESTROY_CASE(RenderPass);
					DURIN_VK_DESTROY_CASE(Buffer);
					DURIN_VK_DESTROY_CASE(BufferView);
					DURIN_VK_DESTROY_CASE(Image);
					DURIN_VK_DESTROY_CASE(ImageView);
					DURIN_VK_DESTROY_CASE(Pipeline);
					DURIN_VK_DESTROY_CASE(PipelineLayout);
					DURIN_VK_DESTROY_CASE(DescriptorPool);
					DURIN_VK_DESTROY_CASE(Framebuffer);
					DURIN_VK_DESTROY_CASE(DescriptorSetLayout);
					DURIN_VK_DESTROY_CASE(Sampler);
					DURIN_VK_DESTROY_CASE(Semaphore);
					DURIN_VK_DESTROY_CASE(ShaderModule);
					DURIN_VK_DESTROY_CASE(Event);
					// TODO: Others
				default:
					DURIN_ERROR("Unknown Vulkan resource type {} in deferred deletion queue", static_cast<uint32>(Entry.Type));
					break;
				}
			}
		}
	}

	FVulkanDevice::FVulkanDevice(FVulkanDynamicRHI* InRHI, vk::PhysicalDevice InGpu)
		: RHI(InRHI)
		, Gpu(InGpu)
		, FenceManager({*this})
		, DeferredDeletionQueue(this)
	{
	}

	FVulkanDevice::~FVulkanDevice()
	{
		Destroy();
	}

	void FVulkanDevice::InitGpu()
	{
		GpuProps = Gpu.getProperties();
		DURIN_DEBUG("Vulkan Device Information:");
		DURIN_DEBUG("- Device Name: {}", GpuProps.deviceName.data());
		DURIN_TRACE("- Device Type: {}", vk::to_string(GpuProps.deviceType));
		DURIN_DEBUG("- API Version: {}.{}.{} (0x{:x})", vk::apiVersionMajor(GpuProps.apiVersion), vk::apiVersionMinor(GpuProps.apiVersion), vk::apiVersionPatch(GpuProps.apiVersion), GpuProps.apiVersion);
		DURIN_TRACE("- Vendor ID: 0x{:x}", GpuProps.vendorID);
		DURIN_DEBUG("- Driver Version: 0x{:x}", GpuProps.driverVersion);

		QueueFamilyProps = Gpu.getQueueFamilyProperties();

		const FVulkanDeviceExtensionArray SupportedDeviceExtensions = FVulkanDeviceExtension::GetDurinSupportedDeviceExtensions(this);

		CreateDevice(SupportedDeviceExtensions);
		MemoryManager.Init(this);

		ImmediateContext = new FVulkanCommandListContext(RHI, *this, GraphicsQueue);

		RenderPassManager = new FVulkanRenderPassManager(*this);
		PipelineManager = new FVulkanPipelineStateCacheManager(*this);
		DescriptorSetCache = new FVulkanDescriptorSetLayoutCache(*this);
		GlobalDescriptorPool = new FVulkanGlobalDescriptorPool(*this);
		DynamicUniformBufferAllocator = new FVulkanDynamicUniformBufferAllocator(*this);

		for (auto& Frame : Frames)
		{
			Frame = new FVulkanFrame(*this);
		}
	}

	auto FVulkanDevice::CreateDevice(const FVulkanDeviceExtensionArray& InDeviceExtensions) -> void
	{
		assert(Device == VK_NULL_HANDLE);

		for (const std::unique_ptr<FVulkanDeviceExtension>& Extension : InDeviceExtensions)
		{
			if (Extension->InUse())
			{
				DeviceExtensions.push_back(Extension->GetExtensionName());
			}
		}

		int32 GraphicsQueueIndex = -1;
		int32 ComputeQueueIndex = -1;
		int32 TransferQueueIndex = -1;

		std::vector<vk::DeviceQueueCreateInfo> QueueCreateInfos;
		const std::vector<float> QueuePriorities = {1.0f};

		for (int32 FamilyIndex = 0; FamilyIndex < QueueFamilyProps.size(); ++FamilyIndex)
		{
			const vk::QueueFamilyProperties& QueueFamilyProp = QueueFamilyProps[FamilyIndex];

			bool bIsValidQueue = false;

			DURIN_TRACE("Queue Family {}:", FamilyIndex);
			DURIN_TRACE("- Queue Count: {}", QueueFamilyProp.queueCount);
			DURIN_TRACE("- Queue Flags: {}", vk::to_string(QueueFamilyProp.queueFlags));
			DURIN_TRACE("- Timestamp Valid Bits: {}", QueueFamilyProp.timestampValidBits);
			DURIN_TRACE("- Min Image Transfer Granularity: ({}, {}, {})", QueueFamilyProp.minImageTransferGranularity.width, QueueFamilyProp.minImageTransferGranularity.height, QueueFamilyProp.minImageTransferGranularity.depth);

			if (QueueFamilyProp.queueFlags & vk::QueueFlagBits::eGraphics)
			{
				GraphicsQueueIndex = FamilyIndex;
				bIsValidQueue = true;
			}

			if (QueueFamilyProp.queueFlags & vk::QueueFlagBits::eCompute)
			{
				if (ComputeQueueIndex == -1 && FamilyIndex != GraphicsQueueIndex)
				{
					ComputeQueueIndex = FamilyIndex;
					bIsValidQueue = true;
				}
			}

			if (QueueFamilyProp.queueFlags & vk::QueueFlagBits::eTransfer)
			{
				// Prefer a specialized transfer queue
				if (TransferQueueIndex == -1 && !(QueueFamilyProp.queueFlags & vk::QueueFlagBits::eGraphics) && !(QueueFamilyProp.queueFlags & vk::QueueFlagBits::eCompute))
				{
					TransferQueueIndex = FamilyIndex;
					bIsValidQueue = true;
				}
			}

			if (!bIsValidQueue)
			{
				DURIN_TRACE("Skipping unnecessary Queue Family {}", FamilyIndex);
				continue;
			}

			QueueCreateInfos.emplace_back();
			vk::DeviceQueueCreateInfo& CurrQueueCreateInfo = QueueCreateInfos.back();
			CurrQueueCreateInfo.queueFamilyIndex = FamilyIndex;
			CurrQueueCreateInfo.queueCount = 1;
			CurrQueueCreateInfo.pQueuePriorities = &(QueuePriorities[0]);
		}

		vk::PhysicalDeviceFeatures DeviceFeatures;
		DeviceFeatures.fillModeNonSolid = vk::True;

		vk::DeviceCreateInfo DeviceInfo;
		DeviceInfo.setQueueCreateInfos(QueueCreateInfos);
		DeviceInfo.setPEnabledFeatures(&DeviceFeatures);
		DeviceInfo.setEnabledExtensionCount(static_cast<uint32>(DeviceExtensions.size()));
		DeviceInfo.setPpEnabledExtensionNames(DeviceExtensions.data());

		try
		{
			Device = Gpu.createDevice(DeviceInfo);
			DURIN_TRACE("Vulkan device created");
		}
		catch (const std::runtime_error& err)
		{
			DURIN_ERROR("Failed to create Vulkan device: {}", err.what());
		}

		DURIN_TRACE("Queue Indexes:");
		DURIN_TRACE("Graphics Queue Index: {}", GraphicsQueueIndex);
		GraphicsQueue = new FVulkanQueue(this, GraphicsQueueIndex);

		if (ComputeQueueIndex == -1)
		{
			ComputeQueueIndex = GraphicsQueueIndex;
		}
		DURIN_TRACE("Compute Queue Index: {}", ComputeQueueIndex);
		ComputeQueue = new FVulkanQueue(this, ComputeQueueIndex);

		if (TransferQueueIndex == -1)
		{
			TransferQueueIndex = ComputeQueueIndex;
		}
		DURIN_TRACE("Transfer Queue Index: {}", TransferQueueIndex);
		TransferQueue = new FVulkanQueue(this, TransferQueueIndex);
	}

	auto FVulkanDevice::SetupPresentQueue(vk::SurfaceKHR InSurface) -> void
	{
		uint32 QueueFamilyIndex = INDEX_NONE_U32;
		for (uint32 FamilyIndex = 0; FamilyIndex < QueueFamilyProps.size(); ++FamilyIndex)
		{
			if (Gpu.getSurfaceSupportKHR(FamilyIndex, InSurface))
			{
				QueueFamilyIndex = FamilyIndex;
				break;
			}
		}

		if (QueueFamilyIndex == INDEX_NONE_U32)
		{
			DURIN_ERROR("Failed to find a queue family that supports presentation");
			return;
		}

		if (PresentQueue != nullptr)
		{
			if (PresentQueue->GetFamilyIndex() == QueueFamilyIndex)
			{
				return;
			}
			delete PresentQueue;
			PresentQueue = nullptr;
		}

		PresentQueue = new FVulkanQueue(this, QueueFamilyIndex);
	}

	auto FVulkanDevice::WaitUtilIdle() const -> void
	{
		Device.waitIdle();
	}

	vk::Device FVulkanDevice::GetHandle() const
	{
		return Device;
	}

	vk::PhysicalDevice FVulkanDevice::GetGpu() const
	{
		return Gpu;
	}

	auto FVulkanDevice::GetRenderPassManager() const -> FVulkanRenderPassManager&
	{
		check(RenderPassManager);
		// ReSharper disable once CppDFANullDereference
		return *RenderPassManager;
	}

	auto FVulkanDevice::AcquireDeferredContext() -> FVulkanCommandListContext*
	{
		if (CommandContexts.empty())
		{
			return new FVulkanCommandListContext(GVulkanRHI, *this, GraphicsQueue);
		}
		FVulkanCommandListContext* Context = CommandContexts.back();

		return Context;
	}

	auto FVulkanDevice::ReleaseDeferredContext(FVulkanCommandListContext* Context) -> void
	{
		CommandContexts.push_back(Context);
	}

	auto FVulkanDevice::GetCurrentFrame() -> FVulkanFrame&
	{
		return *Frames[GRenderFrameCounterRenderThread % Frames.size()];
	}

	auto FVulkanDevice::NotifyDeleted_Image(vk::Image Image) -> void
	{
		GetRenderPassManager().NotifyDeleted_Image(Image);
	}

	auto FVulkanDevice::Destroy() -> void
	{
		if (Device)
		{
			WaitUtilIdle();
		}

		for (auto*& Frame : Frames)
		{
			if (Frame)
			{
				Frame->ReleaseInFlightPayloadsAfterDeviceIdle();
			}
		}

		delete ImmediateContext;
		ImmediateContext = nullptr;

		for (FVulkanCommandListContext* Context : CommandContexts)
		{
			delete Context;
		}
		CommandContexts.clear();

		for (auto*& Frame : Frames)
		{
			delete Frame;
			Frame = nullptr;
		}
		delete GlobalDescriptorPool;
		GlobalDescriptorPool = nullptr;

		delete DynamicUniformBufferAllocator;
		DynamicUniformBufferAllocator = nullptr;

		delete DescriptorSetCache;
		DescriptorSetCache = nullptr;

		delete PipelineManager;
		PipelineManager = nullptr;
		GCommandListExecutor.GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		DeferredDeletionQueue.Clear();
		MemoryManager.Deinit();

		delete GraphicsQueue;
		GraphicsQueue = nullptr;
		delete TransferQueue;
		TransferQueue = nullptr;
		delete PresentQueue;
		PresentQueue = nullptr;

		delete RenderPassManager;
		RenderPassManager = nullptr;

		FenceManager.Deinit();

		DeferredDeletionQueue.ReleaseResources(true);

		Device.destroy();
		Device = nullptr;
	}
} // namespace Durin::VulkanRHI
