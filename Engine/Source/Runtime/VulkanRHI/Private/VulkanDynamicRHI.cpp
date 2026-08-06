#include "VulkanDynamicRHI.h"

#include "VulkanContext.h"
#include "VulkanExtensions.h"
#include "VulkanDevice.h"
#include "VulkanSubmission.h"
#include "VulkanCommandBuffer.h"
#include "VulkanBuffer.h"
#include "VulkanRHIPrivate.h"

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
		VULKAN_HPP_DEFAULT_DISPATCHER.init(::vkGetInstanceProcAddr);
	}

	FVulkanDynamicRHI::~FVulkanDynamicRHI()
	{
		if (GVulkanRHI == this)
		{
			GVulkanRHI = nullptr;
		}
	}

	auto FVulkanDynamicRHI::Init() -> void
	{
		CheckVulkanRHIThread();
		CreateInstance();
		VULKAN_HPP_DEFAULT_DISPATCHER.init(Instance);
		SelectDevice();
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
		CheckVulkanRHIThread();
		// Render thread should already be stopped at this point.
		delete Device;
		Device = nullptr;
		if (Instance)
		{
			Instance.destroy();
			Instance = nullptr;
		}
	}

	auto FVulkanDynamicRHI::RHIBeginFrame(
		const FRHIBeginFrameArgs& Args) -> void
	{
		CheckVulkanRHIThread();
		const uint32 FrameIndex = static_cast<uint32>(
			Args.FrameNumber % kFrameInFlight);
		Device->SetCurrentFrameIndex(FrameIndex);
		FVulkanFrame& Frame = Device->GetCurrentFrame();
		Frame.Prepare();
		Device->GetGlobalDescriptorPool().ResetPoolsForCurrentFrame();
		Device->GetImmediateContext()->RHIBeginFrame(Args);
	}

	auto FVulkanDynamicRHI::RHIBeginFrame_RenderThread(
		FRHICommandListImmediate& RHICmdList) -> void
	{
		check(!GRHIThread || IsInRenderingThread());
		FDynamicRHI::RHIBeginFrame_RenderThread(RHICmdList);
		Device->GetDynamicUniformBufferAllocator().BeginFrameProducer(
			static_cast<uint32>(
				GCommandListExecutor.GetFrameNumber() % kFrameInFlight));
	}

	auto FVulkanDynamicRHI::RHIEndFrame() -> void
	{
		CheckVulkanRHIThread();
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
		CheckVulkanRHIThread();
		return Device->GetHandle();
	}

	auto FVulkanDynamicRHI::RHIGetVkInstance() const -> vk::Instance
	{
		CheckVulkanRHIThread();
		return Instance;
	}

	auto FVulkanDynamicRHI::RHIGetVkPhysicalDevice() const -> vk::PhysicalDevice
	{
		CheckVulkanRHIThread();
		return Device->GetGpu();
	}

	auto FVulkanDynamicRHI::RHIExecuteCommandBufferForBackendIntegration(
		std::function<void(vk::CommandBuffer)> Operation) -> void
	{
		check(Operation);
		GCommandListExecutor.ExecuteSynchronousOperation(true,
			[this, Operation = std::move(Operation)]() mutable {
				CheckVulkanRHIThread();
				Operation(Device->GetImmediateContext()
					->GetCommandBuffer()->GetHandle());
			});
	}

	auto FVulkanDynamicRHI::RHIAllocateDynamicUniformBuffer(
		FRHICommandListImmediate& RHICmdList,
		const void* Data,
		uint32 Size) -> FRHIUniformBufferRange
	{
		check(Data && Size != 0);
		const uint32 FrameIndex = static_cast<uint32>(
			GCommandListExecutor.GetFrameNumber() % kFrameInFlight);
		auto& Allocator = Device->GetDynamicUniformBufferAllocator();
		FRHIUniformBufferRange Result;
		if (Allocator.TryAllocate(FrameIndex, Data, Size, Result))
		{
			return Result;
		}

		GCommandListExecutor.ExecuteSynchronousOperation(true,
			[&Allocator, FrameIndex, Size]() {
				Allocator.ReservePage(FrameIndex, Size);
			});
		checkf(Allocator.TryAllocate(FrameIndex, Data, Size, Result),
			"A prepared dynamic-uniform overflow page must satisfy the pending allocation.");
		return Result;
	}

	auto FVulkanDynamicRHI::CreateInstance() -> void
	{
		CheckVulkanRHIThread();
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
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Instance);
#endif
			vk::Instance InstanceCandidate = vk::createInstance(InstanceInfo);
			Instance = InstanceCandidate;
		}
		catch (const vk::SystemError& err)
		{
			throw std::runtime_error(std::format(
				"Vulkan instance creation failed: result={}, extensions={}, layers={}, error={}",
				vk::to_string(static_cast<vk::Result>(err.code().value())),
				InstanceExtensions.size(), InstanceLayers.size(), err.what()));
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
		CheckVulkanRHIThread();
		std::vector<vk::PhysicalDevice> Gpus = Instance.enumeratePhysicalDevices();

		if (Gpus.empty())
		{
			throw std::runtime_error(
				"Vulkan physical-device selection failed: the driver reported no devices.");
		}

		std::multimap<int, vk::PhysicalDevice> GpuScores;

		for (const auto& Gpu : Gpus)
		{
			int Score = RateDeviceSuitability(Gpu);

			GpuScores.insert(std::make_pair(Score, Gpu));
		}

		auto DeviceCandidate =
			std::make_unique<FVulkanDevice>(this, GpuScores.rbegin()->second);
		DeviceCandidate->InitGpu(static_cast<uint32>(InstanceExtensions.size()));
		Device = DeviceCandidate.release();
	}

	auto FVulkanDynamicRHI::SetupInstanceLayers(const FVulkanInstanceExtensionArray& DurinExtensions) -> void
	{
		// TODO: Implement this function.
		// For now, just return the validation layer.
		std::vector<const char*> Layers = {"VK_LAYER_KHRONOS_validation"};
		InstanceLayers = Layers;
	}

}
