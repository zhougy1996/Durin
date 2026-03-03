#include "VulkanDevice.h"

#include "VulkanDynamicRHI.h"
#include "VulkanExtensions.h"
#include "VulkanContext.h"
#include "VulkanRenderPass.h"
#include "VulkanPipeline.h"
#include "VulkanQueue.h"

namespace Doge::VulkanRHI
{
	FVulkanDevice::FVulkanDevice(FVulkanDynamicRHI* InRHI, vk::PhysicalDevice Gpu)
		: RHI(InRHI)
		, Gpu(Gpu)
		, FenceManager({*this})
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
		DOGE_DEBUG("- Device Type: {}", vk::to_string(GpuProps.deviceType));
		DOGE_DEBUG("- API Version: {}.{}.{} (0x{:x})", vk::apiVersionMajor(GpuProps.apiVersion), vk::apiVersionMinor(GpuProps.apiVersion), vk::apiVersionPatch(GpuProps.apiVersion), GpuProps.apiVersion);
		DOGE_DEBUG("- Vendor ID: 0x{:x}", GpuProps.vendorID);
		DOGE_DEBUG("- Driver Version: 0x{:x}", GpuProps.driverVersion);

		QueueFamilyProps = Gpu.getQueueFamilyProperties();

		FVulkanDeviceExtensionArray SupportedDeviceExtensions = FVulkanDeviceExtension::GetDogeSupportedDeviceExtensions(this);

		CreateDevice(SupportedDeviceExtensions);

		ImmediateContext = new FVulkanCommandListContext(RHI, *this, GraphicsQueue);

		RenderPassManager = new FVulkanRenderPassManager(*this);
		PipelineManager = new FVulkanPipelineManager(*this);
	}

	auto FVulkanDevice::CreateDevice(const FVulkanDeviceExtensionArray& InDeviceExtensions) -> void
	{
		assert(Device == VK_NULL_HANDLE);

		for (const TUniquePtr<FVulkanDeviceExtension>& Extension : InDeviceExtensions)
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
		std::vector<float> QueuePriorities = {1.0f};

		for (int32 FamilyIndex = 0; FamilyIndex < QueueFamilyProps.size(); ++FamilyIndex)
		{
			const vk::QueueFamilyProperties& QueueFamilyProp = QueueFamilyProps[FamilyIndex];

			bool bIsValidQueue = false;

			DOGE_DEBUG("Queue Family {}:", FamilyIndex);
			DOGE_DEBUG("- Queue Count: {}", QueueFamilyProp.queueCount);
			DOGE_DEBUG("- Queue Flags: {}", vk::to_string(QueueFamilyProp.queueFlags));
			DOGE_DEBUG("- Timestamp Valid Bits: {}", QueueFamilyProp.timestampValidBits);
			DOGE_DEBUG("- Min Image Transfer Granularity: ({}, {}, {})", QueueFamilyProp.minImageTransferGranularity.width, QueueFamilyProp.minImageTransferGranularity.height, QueueFamilyProp.minImageTransferGranularity.depth);

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
				DOGE_DEBUG("Skipping unnecessary Queue Family {}", FamilyIndex);
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
			DOGE_INFO("Vulkan device created");
		}
		catch (const std::runtime_error& err)
		{
			DOGE_ERROR("Failed to create Vulkan device: {}", err.what());
		}

		DOGE_DEBUG("Queue Indexes:");
		DOGE_DEBUG("Graphics Queue Index: {}", GraphicsQueueIndex);
		GraphicsQueue = new FVulkanQueue(this, GraphicsQueueIndex);

		if (ComputeQueueIndex == -1)
		{
			ComputeQueueIndex = GraphicsQueueIndex;
		}
		DOGE_DEBUG("Compute Queue Index: {}", ComputeQueueIndex);
		ComputeQueue = new FVulkanQueue(this, ComputeQueueIndex);

		if (TransferQueueIndex == -1)
		{
			TransferQueueIndex = ComputeQueueIndex;
		}
		DOGE_DEBUG("Transfer Queue Index: {}", TransferQueueIndex);
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

	auto FVulkanDevice::WaitUtilIdle() -> void
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

	auto FVulkanDevice::GetRenderPassManager() -> FVulkanRenderPassManager&
	{
		return *RenderPassManager;
	}

	auto FVulkanDevice::AcquireDeferredContext() -> FVulkanCommandListContext*
	{
		if (CommandContexts.empty())
		{
			return new FVulkanCommandListContext(GVulkanRHI, *this, GraphicsQueue);
		}
		FVulkanCommandListContext* Context = CommandContexts.back();
		// CommandContexts_.pop_back();

		return Context;
	}

	auto FVulkanDevice::ReleaseDeferredContext(FVulkanCommandListContext* Context) -> void
	{
		CommandContexts.push_back(Context);
	}

	auto FVulkanDevice::Destroy() -> void
	{
		delete RenderPassManager;
		RenderPassManager = nullptr;

		Device.destroy();
	}
}