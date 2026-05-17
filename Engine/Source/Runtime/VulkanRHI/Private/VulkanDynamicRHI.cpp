#include "VulkanDynamicRHI.h"

#include "VulkanContext.h"
#include "VulkanExtensions.h"
#include "VulkanDevice.h"
#include "VulkanSubmission.h"
#include "VulkanCommandBuffer.h"

#include "VulkanDescriptorSets.h"

// Define the default dispatch loader storage for Vulkan-Hpp. This will allow us to load Vulkan functions at runtime.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace Durin::VulkanRHI
{
	FVulkanDynamicRHI* GVulkanRHI = nullptr;

	IMPLEMENT_MODULE(FVulkanDynamicRHIModule, VulkanRHI)

	FVulkanDynamicRHI::FVulkanDynamicRHI()
	{
		VULKAN_HPP_DEFAULT_DISPATCHER.init(DynamicLoader);
	}

	auto FVulkanDynamicRHI::Init() -> void
	{
		CreateInstance();
		VULKAN_HPP_DEFAULT_DISPATCHER.init(Instance);
		SelectDevice();
		Device->InitGpu();
		VULKAN_HPP_DEFAULT_DISPATCHER.init(Device->GetHandle());
	}

	auto FVulkanDynamicRHI::Shutdown() -> void
	{
		// Render thread should already be stopped at this point.
		delete Device;
		Instance.destroy();
	}

	auto FVulkanDynamicRHI::RHIBeginFrame() -> void
	{
		FVulkanFrame& Frame = Device->GetCurrentFrame();
		Frame.Prepare();
		Device->GetGlobalDescriptorPool().ResetPoolsForCurrentFrame();
	}

	auto FVulkanDynamicRHI::RHIEndFrame() -> void
	{
		auto& Context = FRHICommandListImmediate::Get().GetContext();
		Context.RHIEndFrame();
	}

	auto FVulkanDynamicRHI::RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void
	{
		FDynamicRHI::RHIEndFrame_RenderThread(RHICmdList);
		GVulkanRHIDeletionFrameNumber++;

		Device->GetDeferredDeletionQueue().ReleaseResources();
	}

	auto FVulkanDynamicRHI::RHIGetVkDevice() const -> vk::Device
	{
		return Device->GetHandle();
	}

	auto FVulkanDynamicRHI::RHIGetVkInstance() const -> vk::Instance
	{
		return Instance;
	}

	auto FVulkanDynamicRHI::RHIGetVkPhysicalDevice() const -> vk::PhysicalDevice
	{
		return Device->GetGpu();
	}

	auto FVulkanDynamicRHI::RHIGetVkCommandBuffer(FRHICommandListBase& RHICmdList) const -> vk::CommandBuffer
	{
		auto& Context = static_cast<FVulkanCommandListContext&>(RHICmdList.GetContext());
		return Context.GetCommandBuffer()->GetHandle();
	}

	auto FVulkanDynamicRHI::RHIBlockUntilGPUIdle() -> void
	{

		Device->WaitUtilIdle();
	}

	auto FVulkanDynamicRHI::CreateInstance() -> void
	{
		std::string EngineName = "Doge";

		// Create application info
		vk::ApplicationInfo AppInfo(EngineName.c_str(), VK_MAKE_VERSION(1, 0, 0), "Doge Engine", VK_MAKE_VERSION(1, 0, 0), VK_API_VERSION_1_3);

		// Get instance extensions
		FVulkanInstanceExtensionArray DogeInstanceExtensions = FVulkanInstanceExtension::GetDogeSupportedInstanceExtensions();

		for (const auto& Extension : DogeInstanceExtensions)
		{
			if (Extension->InUse())
			{
				InstanceExtensions.push_back(Extension->GetExtensionName());
			}
		}

		SetupInstanceLayers(DogeInstanceExtensions);

		vk::InstanceCreateInfo InstanceInfo({}, &AppInfo);

		InstanceInfo.enabledExtensionCount = static_cast<uint32>(InstanceExtensions.size());
		InstanceInfo.ppEnabledExtensionNames = InstanceExtensions.data();

		InstanceInfo.enabledLayerCount = static_cast<uint32>(InstanceLayers.size());
		InstanceInfo.ppEnabledLayerNames = InstanceInfo.enabledLayerCount > 0 ? InstanceLayers.data() : nullptr;

#ifdef __APPLE__
		InstanceInfo.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#endif
		try
		{
			Instance = vk::createInstance(InstanceInfo);
		}
		catch (const vk::SystemError& err)
		{
			DURIN_ERROR("Failed to create Vulkan instance: {}", err.what());
		}

		if (Instance)
		{
			DURIN_TRACE("Vulkan instance created.");
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
		std::vector<vk::PhysicalDevice> Gpus = Instance.enumeratePhysicalDevices();

		if (Gpus.empty())
		{
			DURIN_ERROR("No physical device found.");
			return;
		}

		std::multimap<int, vk::PhysicalDevice> GpuScores;

		for (const auto& Gpu : Gpus)
		{
			int Score = RateDeviceSuitability(Gpu);

			GpuScores.insert(std::make_pair(Score, Gpu));
		}

		Device = new FVulkanDevice(this, GpuScores.rbegin()->second);
	}

	auto FVulkanDynamicRHI::SetupInstanceLayers(const FVulkanInstanceExtensionArray& DogeExtensions) -> void
	{
		// TODO: Implement this function.
		// For now, just return the validation layer.
		std::vector<const char*> Layers = {"VK_LAYER_KHRONOS_validation"};
		InstanceLayers = Layers;
	}

}