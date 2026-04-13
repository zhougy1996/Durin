#include "VulkanDevice.h"


#include "RHICommandList.h"
#include "VulkanDynamicRHI.h"
#include "VulkanExtensions.h"
#include "VulkanContext.h"
#include "VulkanRenderPass.h"
#include "VulkanPipeline.h"
#include "VulkanQueue.h"
#include "VulkanRHIPrivate.h"

namespace Doge::VulkanRHI
{
	uint64 GVulkanRHIDeletionFrameNumber = 0;

	constexpr uint64 GVulkanNumFramesToWaitForResourceDelete =  kFrameInFlight;

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

#define DOGE_VK_DESTROY_CASE(Type, ...)                                                 \
	case EType::Type:                                                                   \
		__VA_ARGS__                                                                     \
		DeviceHandle.destroy##Type(vk::Type{reinterpret_cast<Vk##Type>(Entry.Handle)}); \
		break

#define DOGE_VMA_DESTROY_CASE(Type, ...)                                                                   \
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
					DOGE_VMA_DESTROY_CASE(Image);
					DOGE_VMA_DESTROY_CASE(Buffer);
				default:
					DOGE_ERROR("Unknown Vulkan resource type {} for vma", static_cast<uint32>(Entry.Type));
					break;
				}
			}
			else
			{
				switch (Entry.Type)
				{
					DOGE_VK_DESTROY_CASE(RenderPass);
					DOGE_VK_DESTROY_CASE(Buffer);
					DOGE_VK_DESTROY_CASE(BufferView);
					DOGE_VK_DESTROY_CASE(Image);
					DOGE_VK_DESTROY_CASE(ImageView);
					DOGE_VK_DESTROY_CASE(Pipeline);
					DOGE_VK_DESTROY_CASE(PipelineLayout);
					DOGE_VK_DESTROY_CASE(Framebuffer);
					DOGE_VK_DESTROY_CASE(DescriptorSetLayout);
					DOGE_VK_DESTROY_CASE(Sampler);
					DOGE_VK_DESTROY_CASE(Semaphore);
					DOGE_VK_DESTROY_CASE(ShaderModule);
					DOGE_VK_DESTROY_CASE(Event);
					// TODO: Others
				default:
					DOGE_ERROR("Unknown Vulkan resource type {} in deferred deletion queue", static_cast<uint32>(Entry.Type));
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
		DOGE_DEBUG("Vulkan Device Information:");
		DOGE_DEBUG("- Device Name: {}", GpuProps.deviceName.data());
		DOGE_TRACE("- Device Type: {}", vk::to_string(GpuProps.deviceType));
		DOGE_DEBUG("- API Version: {}.{}.{} (0x{:x})", vk::apiVersionMajor(GpuProps.apiVersion), vk::apiVersionMinor(GpuProps.apiVersion), vk::apiVersionPatch(GpuProps.apiVersion), GpuProps.apiVersion);
		DOGE_TRACE("- Vendor ID: 0x{:x}", GpuProps.vendorID);
		DOGE_DEBUG("- Driver Version: 0x{:x}", GpuProps.driverVersion);

		QueueFamilyProps = Gpu.getQueueFamilyProperties();

		const FVulkanDeviceExtensionArray SupportedDeviceExtensions = FVulkanDeviceExtension::GetDogeSupportedDeviceExtensions(this);

		CreateDevice(SupportedDeviceExtensions);
		MemoryManager.Init(this);

		ImmediateContext = new FVulkanCommandListContext(RHI, *this, GraphicsQueue);

		RenderPassManager = new FVulkanRenderPassManager(*this);
		PipelineManager = new FVulkanPipelineManager(*this);

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

			DOGE_TRACE("Queue Family {}:", FamilyIndex);
			DOGE_TRACE("- Queue Count: {}", QueueFamilyProp.queueCount);
			DOGE_TRACE("- Queue Flags: {}", vk::to_string(QueueFamilyProp.queueFlags));
			DOGE_TRACE("- Timestamp Valid Bits: {}", QueueFamilyProp.timestampValidBits);
			DOGE_TRACE("- Min Image Transfer Granularity: ({}, {}, {})", QueueFamilyProp.minImageTransferGranularity.width, QueueFamilyProp.minImageTransferGranularity.height, QueueFamilyProp.minImageTransferGranularity.depth);

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
				DOGE_TRACE("Skipping unnecessary Queue Family {}", FamilyIndex);
				continue;
			}

			QueueCreateInfos.emplace_back();
			vk::DeviceQueueCreateInfo& CurrQueueCreateInfo = QueueCreateInfos.back();
			CurrQueueCreateInfo.queueFamilyIndex = FamilyIndex;
			CurrQueueCreateInfo.queueCount = 1;
			CurrQueueCreateInfo.pQueuePriorities = &(QueuePriorities[0]);
		}

		vk::PhysicalDeviceFeatures DeviceFeatures;

		vk::DeviceCreateInfo DeviceInfo;
		DeviceInfo.setQueueCreateInfos(QueueCreateInfos);
		DeviceInfo.setPEnabledFeatures(&DeviceFeatures);
		DeviceInfo.setEnabledExtensionCount(static_cast<uint32>(DeviceExtensions.size()));
		DeviceInfo.setPpEnabledExtensionNames(DeviceExtensions.data());

		try
		{
			Device = Gpu.createDevice(DeviceInfo);
			DOGE_TRACE("Vulkan device created");
		}
		catch (const std::runtime_error& err)
		{
			DOGE_ERROR("Failed to create Vulkan device: {}", err.what());
		}

		DOGE_TRACE("Queue Indexes:");
		DOGE_TRACE("Graphics Queue Index: {}", GraphicsQueueIndex);
		GraphicsQueue = new FVulkanQueue(this, GraphicsQueueIndex);

		if (ComputeQueueIndex == -1)
		{
			ComputeQueueIndex = GraphicsQueueIndex;
		}
		DOGE_TRACE("Compute Queue Index: {}", ComputeQueueIndex);
		ComputeQueue = new FVulkanQueue(this, ComputeQueueIndex);

		if (TransferQueueIndex == -1)
		{
			TransferQueueIndex = ComputeQueueIndex;
		}
		DOGE_TRACE("Transfer Queue Index: {}", TransferQueueIndex);
		TransferQueue = new FVulkanQueue(this, TransferQueueIndex);
	}

	auto FVulkanDevice::SetupPresentQueue(vk::SurfaceKHR InSurface) -> void
	{
		uint32 QueueFamilyIndex = UINT32_MAX;
		for (uint32 FamilyIndex = 0; FamilyIndex < QueueFamilyProps.size(); ++FamilyIndex)
		{
			if (Gpu.getSurfaceSupportKHR(FamilyIndex, InSurface))
			{
				QueueFamilyIndex = FamilyIndex;
				break;
			}
		}

		if (QueueFamilyIndex == UINT32_MAX)
		{
			DOGE_ERROR("Failed to find a queue family that supports presentation");
			return;
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
		return *Frames[GFrameCounterRenderThread % Frames.size()];
	}

	auto FVulkanDevice::Destroy() -> void
	{
		for (auto*& Frame : Frames)
		{
			delete Frame;
			Frame = nullptr;
		}

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

		delete ImmediateContext;
		ImmediateContext = nullptr;

		FenceManager.Deinit();

		DeferredDeletionQueue.ReleaseResources(true);

		Device.destroy();
		Device = nullptr;
	}
} // namespace Doge::VulkanRHI