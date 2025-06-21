#include "VulkanDynamicRHI.h"

#include "VulkanExtensions.h"
#include "VulkanDevice.h"

FVulkanDynamicRHI* GVulkanRHI = nullptr;

IMPLEMENT_MODULE(FVulkanDynamicRHIModule, VulkanRHI)

FVulkanDynamicRHI::FVulkanDynamicRHI()
{
	CreateInstance();
	SelectDevice();
}

auto FVulkanDynamicRHI::Init() -> void
{
	Device_->InitGpu();
}

auto FVulkanDynamicRHI::Shutdown() -> void
{
	delete Device_;
}

auto FVulkanDynamicRHI::RHIGetVkDevice() const -> vk::Device
{
	return Device_->GetHandle();
}

auto FVulkanDynamicRHI::RHIGetVkInstance() const -> vk::Instance
{
	return Instance_;
}

auto FVulkanDynamicRHI::RHIGetVkPhysicalDevice() const -> vk::PhysicalDevice
{
	return Device_->GetGpu();
}

auto FVulkanDynamicRHI::CreateInstance() -> void
{
	FString EngineName = "Doge";

	// Create application info
	vk::ApplicationInfo AppInfo(EngineName.c_str(), VK_MAKE_VERSION(1, 0, 0), "Doge Engine", VK_MAKE_VERSION(1, 0, 0), VK_API_VERSION_1_0);

	// Get instance extensions
	FVulkanInstanceExtensionArray DogeInstanceExtensions = FVulkanInstanceExtension::GetDogeSupportedInstanceExtensions();

	for (const auto& Extension : DogeInstanceExtensions)
	{
		if (Extension->InUse())
		{
			InstanceExtensions_.push_back(Extension->GetExtensionName());
		}
	}

	SetupInstanceLayers(DogeInstanceExtensions);

	vk::InstanceCreateInfo InstanceInfo({}, &AppInfo);

	InstanceInfo.enabledExtensionCount = static_cast<uint32>(InstanceExtensions_.size());
	InstanceInfo.ppEnabledExtensionNames = InstanceExtensions_.data();

	InstanceInfo.enabledLayerCount = static_cast<uint32>(InstanceLayers_.size());
	InstanceInfo.ppEnabledLayerNames = InstanceInfo.enabledLayerCount > 0 ? InstanceLayers_.data() : nullptr;

	try
	{
		Instance_ = vk::createInstance(InstanceInfo);
	}
	catch (const vk::SystemError& err)
	{
		DOGE_ERROR("Failed to create Vulkan instance: {}", err.what());
	}

	if (Instance_)
	{
		DOGE_INFO("Vulkan instance created.");
	}
}

static auto RateDeviceSuitability(vk::PhysicalDevice Device) -> int
{
	vk::PhysicalDeviceProperties DeviceProperties = Device.getProperties();
	vk::PhysicalDeviceFeatures DeviceFeatures = Device.getFeatures();

	int Score = 0;

	if (DeviceProperties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
	{
		Score += 1000;
	}

	Score += DeviceProperties.limits.maxImageDimension2D;

	if (!DeviceFeatures.geometryShader)
	{
		return 0;
	}

	return Score;
}

auto FVulkanDynamicRHI::SelectDevice() -> void
{
	TArray<vk::PhysicalDevice> Gpus = Instance_.enumeratePhysicalDevices();

	if (Gpus.empty())
	{
		DOGE_ERROR("No physical device found.");
		return;
	}

	TMultiMap<int, vk::PhysicalDevice> GpuScores;

	for (const auto& Gpu : Gpus)
	{
		int Score = RateDeviceSuitability(Gpu);

		GpuScores.insert(std::make_pair(Score, Gpu));
	}

	Device_ = new FVulkanDevice(this, GpuScores.rbegin()->second);
}

auto FVulkanDynamicRHI::SetupInstanceLayers(const FVulkanInstanceExtensionArray& DogeExtensions) -> void
{
	// TODO: Implement this function.
	// For now, just return the validation layer.
	TArray<const char*> Layers = {"VK_LAYER_KHRONOS_validation"};
	InstanceLayers_ = Layers;
}
