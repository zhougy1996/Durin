#include "VulkanDevice.h"

#include "VulkanDynamicRHI.h"
#include "VulkanExtensions.h"
#include "VulkanContext.h"
#include "VulkanRenderPass.h"
#include "VulkanPipeline.h"
#include "VulkanQueue.h"

FVulkanDevice::FVulkanDevice(FVulkanDynamicRHI* RHI, vk::PhysicalDevice Gpu)
	: RHI_(RHI)
	, Gpu_(Gpu)
	, FenceManager_({*this})
{
}

FVulkanDevice::~FVulkanDevice()
{
	Destroy();
}

void FVulkanDevice::InitGpu()
{
	GpuProps_ = Gpu_.getProperties();
	DOGE_DEBUG("Vulkan Device Information:");
	DOGE_DEBUG("- Device Name: {}", GpuProps_.deviceName.data());
	DOGE_DEBUG("- Device Type: {}", vk::to_string(GpuProps_.deviceType));
	DOGE_DEBUG("- API Version: {}.{}.{} (0x{:x})", vk::apiVersionMajor(GpuProps_.apiVersion), vk::apiVersionMinor(GpuProps_.apiVersion), vk::apiVersionPatch(GpuProps_.apiVersion), GpuProps_.apiVersion);
	DOGE_DEBUG("- Vendor ID: 0x{:x}", GpuProps_.vendorID);
	DOGE_DEBUG("- Driver Version: 0x{:x}", GpuProps_.driverVersion);

	QueueFamilyProps_ = Gpu_.getQueueFamilyProperties();

	FVulkanDeviceExtensionArray DeviceExtensions = FVulkanDeviceExtension::GetDogeSupportedDeviceExtensions(this);

	CreateDevice(DeviceExtensions);

	ImmediateContext_ = new FVulkanCommandListContext(RHI_, *this, GraphicsQueue_);

	RenderPassManager_ = new FVulkanRenderPassManager(*this);
	PipelineManager_ = new FVulkanPipelineManager(*this);
}

auto FVulkanDevice::CreateDevice(FVulkanDeviceExtensionArray& DeviceExtensions) -> void
{
	assert(Device_ == VK_NULL_HANDLE);

	for (const TUniquePtr<FVulkanDeviceExtension>& Extension : DeviceExtensions)
	{
		if (Extension->InUse())
		{
			DeviceExtensions_.push_back(Extension->GetExtensionName());
		}
	}

	int32 GraphicsQueueIndex = -1;
	int32 ComputeQueueIndex = -1;
	int32 TransferQueueIndex = -1;

	std::vector<vk::DeviceQueueCreateInfo> QueueCreateInfos;
	std::vector<float> QueuePriorities = {1.0f};

	for (int32 FamilyIndex = 0; FamilyIndex < QueueFamilyProps_.size(); ++FamilyIndex)
	{
		const vk::QueueFamilyProperties& QueueFamilyProp = QueueFamilyProps_[FamilyIndex];

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
	DeviceInfo.setEnabledExtensionCount(static_cast<uint32>(DeviceExtensions_.size()));
	DeviceInfo.setPpEnabledExtensionNames(DeviceExtensions_.data());

	try
	{
		Device_ = Gpu_.createDevice(DeviceInfo);
		DOGE_INFO("Vulkan device created");
	}
	catch (const std::runtime_error& err)
	{
		DOGE_ERROR("Failed to create Vulkan device: {}", err.what());
	}

	DOGE_DEBUG("Queue Indexes:");
	DOGE_DEBUG("Graphics Queue Index: {}", GraphicsQueueIndex);
	GraphicsQueue_ = new FVulkanQueue(this, GraphicsQueueIndex);

	if (ComputeQueueIndex == -1)
	{
		ComputeQueueIndex = GraphicsQueueIndex;
	}
	DOGE_DEBUG("Compute Queue Index: {}", ComputeQueueIndex);
	ComputeQueue_ = new FVulkanQueue(this, ComputeQueueIndex);

	if (TransferQueueIndex == -1)
	{
		TransferQueueIndex = ComputeQueueIndex;
	}
	DOGE_DEBUG("Transfer Queue Index: {}", TransferQueueIndex);
	TransferQueue_ = new FVulkanQueue(this, TransferQueueIndex);
}

auto FVulkanDevice::SetupPresentQueue(vk::SurfaceKHR Surface) -> void
{
	uint32 QueueFamilyIndex = UINT32_MAX;
	for (uint32 FamilyIndex = 0; FamilyIndex < QueueFamilyProps_.size(); ++FamilyIndex)
	{
		if (Gpu_.getSurfaceSupportKHR(FamilyIndex, Surface))
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

	PresentQueue_ = new FVulkanQueue(this, QueueFamilyIndex);
}

auto FVulkanDevice::WaitUtilIdle() -> void
{
	Device_.waitIdle();
}

vk::Device FVulkanDevice::GetHandle() const
{
	return Device_;
}

vk::PhysicalDevice FVulkanDevice::GetGpu() const
{
	return Gpu_;
}

auto FVulkanDevice::GetRenderPassManager() -> FVulkanRenderPassManager&
{
	return *RenderPassManager_;
}

auto FVulkanDevice::AcquireDeferredContext() -> FVulkanCommandListContext*
{
	if (CommandContexts_.empty())
	{
		return new FVulkanCommandListContext(GVulkanRHI, *this, GraphicsQueue_);
	}
	FVulkanCommandListContext* Context = CommandContexts_.back();
	// CommandContexts_.pop_back();

	return Context;
}

auto FVulkanDevice::ReleaseDeferredContext(FVulkanCommandListContext* Context) -> void
{
	CommandContexts_.push_back(Context);
}

auto FVulkanDevice::Destroy() -> void
{
	delete RenderPassManager_;
	RenderPassManager_ = nullptr;

	Device_.destroy();
}
