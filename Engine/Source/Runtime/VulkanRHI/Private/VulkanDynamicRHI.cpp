#include "VulkanDynamicRHI.h"

#include "RHICommandList.h"
#include "RHIContext.h"
#include "VulkanExtensions.h"
#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"

namespace Doge::VulkanRHI
{
	FVulkanDynamicRHI* GVulkanRHI = nullptr;

	IMPLEMENT_MODULE(FVulkanDynamicRHIModule, VulkanRHI)

	FVulkanDynamicRHI::FVulkanDynamicRHI()
	{
		CreateInstance();
		SelectDevice();
	}

	auto FVulkanDynamicRHI::Init() -> void
	{
		Device->InitGpu();
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
			DOGE_ERROR("Failed to create Vulkan instance: {}", err.what());
		}

		if (Instance)
		{
			DOGE_TRACE("Vulkan instance created.");
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
			DOGE_ERROR("No physical device found.");
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