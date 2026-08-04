#include "VulkanDynamicRHI.h"

#include "VulkanContext.h"
#include "VulkanExtensions.h"
#include "VulkanDevice.h"
#include "VulkanSubmission.h"
#include "VulkanCommandBuffer.h"
#include "VulkanBuffer.h"

#include "VulkanDescriptorSets.h"
#include "Misc/Version.h"

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
		Device->InitGpu(static_cast<uint32>(InstanceExtensions.size()));
		VULKAN_HPP_DEFAULT_DISPATCHER.init(Device->GetHandle());
	}

	auto FVulkanDynamicRHI::IsInstanceExtensionEnabled(const char* ExtensionName) const -> bool
	{
		return std::ranges::any_of(InstanceExtensions, [ExtensionName](const char* EnabledExtension) {
			return strcmp(EnabledExtension, ExtensionName) == 0;
		});
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
		Device->GetDynamicUniformBufferAllocator().BeginFrame(static_cast<uint32>(GRenderFrameCounterRenderThread % kFrameInFlight));
		Device->GetGlobalDescriptorPool().ResetPoolsForCurrentFrame();
		Device->GetImmediateContext()->RHIBeginFrame();
	}

	auto FVulkanDynamicRHI::RHIEndFrame() -> void
	{
		Device->GetImmediateContext()->RHIEndFrame();
		GVulkanRHIDeletionFrameNumber++;
		Device->GetDeferredDeletionQueue().ReleaseResources();
	}

	auto FVulkanDynamicRHI::RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void
	{
		FDynamicRHI::RHIEndFrame_RenderThread(RHICmdList);
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

	auto FVulkanDynamicRHI::RHIGetVkCommandBufferForBackendIntegration() const
		-> vk::CommandBuffer
	{
		return Device->GetImmediateContext()->GetCommandBuffer()->GetHandle();
	}

	auto FVulkanDynamicRHI::CreateInstance() -> void
	{
		std::string EngineName = "Durin";
		const FEngineVersion& EngineVersion = GetEngineVersion();
		const uint32 PackedEngineVersion = VK_MAKE_API_VERSION(0, EngineVersion.Major, EngineVersion.Minor, EngineVersion.Patch);

		// Create application info
		vk::ApplicationInfo AppInfo(EngineName.c_str(), PackedEngineVersion, "Durin Engine", PackedEngineVersion, VK_API_VERSION_1_3);

		// Get instance extensions
		FVulkanInstanceExtensionArray DurinInstanceExtensions = FVulkanInstanceExtension::GetDurinSupportedInstanceExtensions();

		for (const auto& Extension : DurinInstanceExtensions)
		{
			if (Extension->InUse())
			{
				InstanceExtensions.push_back(Extension->GetExtensionName());
			}
		}

		SetupInstanceLayers(DurinInstanceExtensions);

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
			DURIN_ERROR("Failed to create Vulkan instance: result={}, extensions={}, layers={}, error={}",
				vk::to_string(static_cast<vk::Result>(err.code().value())), InstanceExtensions.size(), InstanceLayers.size(), err.what());
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

		if (!DeviceFeatures.geometryShader || !DeviceFeatures.fillModeNonSolid)
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
			DURIN_ERROR("Failed to select a Vulkan physical device: the driver reported no devices.");
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

	auto FVulkanDynamicRHI::SetupInstanceLayers(const FVulkanInstanceExtensionArray& DurinExtensions) -> void
	{
		// TODO: Implement this function.
		// For now, just return the validation layer.
		std::vector<const char*> Layers = {"VK_LAYER_KHRONOS_validation"};
		InstanceLayers = Layers;
	}

}
