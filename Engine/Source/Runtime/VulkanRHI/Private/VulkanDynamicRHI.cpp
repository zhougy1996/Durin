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

		auto ToRHISampleCounts = [](vk::SampleCountFlags Flags) {
			ERHISampleCountFlags Result = ERHISampleCountFlags::None;
			if (Flags & vk::SampleCountFlagBits::e1) Result |= ERHISampleCountFlags::Samples1;
			if (Flags & vk::SampleCountFlagBits::e2) Result |= ERHISampleCountFlags::Samples2;
			if (Flags & vk::SampleCountFlagBits::e4) Result |= ERHISampleCountFlags::Samples4;
			if (Flags & vk::SampleCountFlagBits::e8) Result |= ERHISampleCountFlags::Samples8;
			if (Flags & vk::SampleCountFlagBits::e16) Result |= ERHISampleCountFlags::Samples16;
			return Result;
		};
		const vk::PhysicalDeviceLimits& Limits = Device->GetGpuProperties().limits;
		FRHICapabilities CapabilityCandidate;
		CapabilityCandidate.FeatureLevel = ERHIFeatureLevel::ES3_1;
		CapabilityCandidate.SupportedTextureDimensions =
			ERHITextureDimensionFlags::Texture2D | ERHITextureDimensionFlags::TextureCube;
		CapabilityCandidate.MaxTextureDimension2D = Limits.maxImageDimension2D;
		CapabilityCandidate.MaxTextureDimensionCube = Limits.maxImageDimensionCube;
		CapabilityCandidate.MaxTextureArrayLayers = Limits.maxImageArrayLayers;
		CapabilityCandidate.ColorSampleCounts = ToRHISampleCounts(
			Limits.framebufferColorSampleCounts & Limits.sampledImageColorSampleCounts);
		CapabilityCandidate.DepthSampleCounts = ToRHISampleCounts(
			Limits.framebufferDepthSampleCounts & Limits.sampledImageDepthSampleCounts);
		CapabilityCandidate.MaxColorAttachments = Limits.maxColorAttachments;
		const vk::PhysicalDeviceFeatures Features = Device->GetGpu().getFeatures();
		CapabilityCandidate.bSupportsNonSolidFill = Features.fillModeNonSolid == vk::True;
		CapabilityCandidate.bSupportsDepthClamp = Features.depthClamp == vk::True;
		CapabilityCandidate.bSupportsWideLines = Features.wideLines == vk::True;
		CapabilityCandidate.bSupportsSynchronization2 = Device->SupportsSynchronization2();
		PublishCapabilities(std::move(CapabilityCandidate));
	}

	auto FVulkanDynamicRHI::IsInstanceExtensionEnabled(const char* ExtensionName) const -> bool
	{
		return std::ranges::any_of(InstanceExtensions, [ExtensionName](const std::string& EnabledExtension) {
			return EnabledExtension == ExtensionName;
		});
	}

	auto FVulkanDynamicRHI::Shutdown() -> void
	{
		CheckVulkanRHIThread();
		ClearCapabilities();
		// Render thread should already be stopped at this point.
		delete Device;
		Device = nullptr;
		if (Instance)
		{
			Instance.destroy();
			Instance = nullptr;
		}
		InstanceExtensions.clear();
		InstanceLayers.clear();
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
		requiref(Allocator.TryAllocate(FrameIndex, Data, Size, Result),
			"A prepared dynamic-uniform overflow page must satisfy the pending allocation.");
		return Result;
	}

	auto FVulkanDynamicRHI::CreateInstance() -> void
	{
		CheckVulkanRHIThread();
		std::string EngineName = "Durin";
		const FEngineVersion& EngineVersion = GetEngineVersion();
		const uint32 PackedEngineVersion = VK_MAKE_API_VERSION(0, EngineVersion.Major, EngineVersion.Minor, EngineVersion.Patch);
		FVulkanInstanceNegotiationInput NegotiationInput;
		try
		{
			NegotiationInput.LoaderApiVersion = vk::enumerateInstanceVersion();
			for (const vk::ExtensionProperties& Extension : vk::enumerateInstanceExtensionProperties())
				NegotiationInput.AvailableExtensions.emplace_back(Extension.extensionName.data());
			for (const vk::LayerProperties& Layer : vk::enumerateInstanceLayerProperties())
				NegotiationInput.AvailableLayers.emplace_back(Layer.layerName.data());
		}
		catch (const std::exception& Exception)
		{
			throw std::runtime_error(std::format(
				"Vulkan instance requirement enumeration failed: {}", Exception.what()));
		}
#ifdef _WIN32
		NegotiationInput.PlatformRequiredExtensions.emplace_back(VK_KHR_SURFACE_EXTENSION_NAME);
		NegotiationInput.PlatformRequiredExtensions.emplace_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
		for (const char* RequiredExtension : GMonaRequiredVulkanInstanceExtensions)
		{
			if (std::ranges::find(NegotiationInput.PlatformRequiredExtensions, RequiredExtension)
				== NegotiationInput.PlatformRequiredExtensions.end())
			{
				NegotiationInput.PlatformRequiredExtensions.emplace_back(RequiredExtension);
			}
		}
		const FVulkanValidationPolicy ValidationPolicy = ResolveVulkanValidationPolicy(
			std::getenv("DURIN_VULKAN_VALIDATION"), DURIN_BUILD_DEBUG != 0, DURIN_BUILD_SHIPPING != 0);
		if (ValidationPolicy.bInvalidSetting)
		{
			static std::atomic_bool bLoggedInvalidValidationSetting = false;
			if (!bLoggedInvalidValidationSetting.exchange(true))
			{
				DURIN_ERROR("Invalid DURIN_VULKAN_VALIDATION value; expected 'auto', 'on', or 'off'. Using 'auto'.");
			}
		}
		NegotiationInput.bRequestDiagnostics = ValidationPolicy.bRequestDiagnostics;
		FVulkanInstanceNegotiationResult Negotiation = NegotiateVulkanInstance(NegotiationInput);
		if (!Negotiation.IsSuccess()) throw std::runtime_error(Negotiation.Diagnostic);
		for (const FVulkanRequirementState& Requirement : Negotiation.Requirements)
		{
			if (!Requirement.bRequested || Requirement.bActivated
				|| (Requirement.Class != EVulkanRequirementClass::OptionalFeature
					&& Requirement.Class != EVulkanRequirementClass::OptionalDiagnostic))
			{
				continue;
			}
			static std::mutex DisabledRequirementMutex;
			static std::set<std::string> LoggedDisabledRequirements;
			const std::lock_guard Lock(DisabledRequirementMutex);
			if (LoggedDisabledRequirements.insert(Requirement.Name).second)
			{
				DURIN_DEBUG("Optional Vulkan requirement '{}' is unavailable; startup continues without it.",
					Requirement.Name);
			}
		}

		vk::ApplicationInfo AppInfo(EngineName.c_str(), PackedEngineVersion,
			"Durin Engine", PackedEngineVersion, Negotiation.ApiVersion);
		std::vector<const char*> ExtensionNames;
		std::vector<const char*> LayerNames;
		for (const std::string& Name : Negotiation.EnabledExtensions) ExtensionNames.push_back(Name.c_str());
		for (const std::string& Name : Negotiation.EnabledLayers) LayerNames.push_back(Name.c_str());
		vk::InstanceCreateInfo InstanceInfo({}, &AppInfo);
		InstanceInfo.setPEnabledExtensionNames(ExtensionNames)
			.setPEnabledLayerNames(LayerNames);

#ifdef __APPLE__
		InstanceInfo.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#endif
		try
		{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Instance);
#endif
			vk::Instance InstanceCandidate = vk::createInstance(InstanceInfo);
			InstanceExtensions = std::move(Negotiation.EnabledExtensions);
			InstanceLayers = std::move(Negotiation.EnabledLayers);
			Instance = InstanceCandidate;
		}
		catch (const vk::SystemError& err)
		{
			throw std::runtime_error(std::format(
				"Vulkan instance creation failed: result={}, extensions={}, layers={}, error={}",
				vk::to_string(static_cast<vk::Result>(err.code().value())),
				ExtensionNames.size(), LayerNames.size(), err.what()));
		}
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

		struct FNativeCandidate
		{
			vk::PhysicalDevice Gpu;
			FVulkanPhysicalDeviceCandidateInput Input;
			FVulkanPhysicalDeviceCandidateEvaluation Evaluation;
		};
		std::vector<FNativeCandidate> Candidates;
		Candidates.reserve(Gpus.size());
		for (vk::PhysicalDevice Gpu : Gpus)
		{
			FNativeCandidate Candidate;
			Candidate.Gpu = Gpu;
			const vk::PhysicalDeviceProperties Properties = Gpu.getProperties();
			const vk::PhysicalDeviceFeatures Features = Gpu.getFeatures();
			Candidate.Input.DeviceName = Properties.deviceName.data();
			Candidate.Input.DeviceType = Properties.deviceType;
			Candidate.Input.ApiVersion = Properties.apiVersion;
			Candidate.Input.VendorId = Properties.vendorID;
			Candidate.Input.DeviceId = Properties.deviceID;
			Candidate.Input.MaxImageDimension2D = Properties.limits.maxImageDimension2D;
			Candidate.Input.MaxImageDimensionCube = Properties.limits.maxImageDimensionCube;
			Candidate.Input.MaxImageArrayLayers = Properties.limits.maxImageArrayLayers;
			Candidate.Input.bFillModeNonSolid = Features.fillModeNonSolid == vk::True;
			for (const vk::ExtensionProperties& Extension : Gpu.enumerateDeviceExtensionProperties())
				Candidate.Input.AvailableExtensions.emplace_back(Extension.extensionName.data());

			vk::PhysicalDeviceVulkan11Features Vulkan11Features;
			vk::PhysicalDeviceFeatures2 Features2;
			Features2.setPNext(&Vulkan11Features);
			Gpu.getFeatures2(&Features2);
			Candidate.Input.bShaderDrawParameters = Vulkan11Features.shaderDrawParameters == vk::True;
			if (Properties.apiVersion >= VK_API_VERSION_1_3)
			{
				vk::PhysicalDeviceVulkan13Features Vulkan13Features;
				Features2.setPNext(&Vulkan13Features);
				Gpu.getFeatures2(&Features2);
				Candidate.Input.bSynchronization2Feature = Vulkan13Features.synchronization2 == vk::True;
			}
			else if (std::ranges::find(Candidate.Input.AvailableExtensions,
				VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) != Candidate.Input.AvailableExtensions.end())
			{
				vk::PhysicalDeviceSynchronization2FeaturesKHR Synchronization2Features;
				Features2.setPNext(&Synchronization2Features);
				Gpu.getFeatures2(&Features2);
				Candidate.Input.bSynchronization2Feature = Synchronization2Features.synchronization2 == vk::True;
			}
			if (std::ranges::find(Candidate.Input.AvailableExtensions,
				VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) != Candidate.Input.AvailableExtensions.end())
			{
				vk::PhysicalDeviceSwapchainMaintenance1FeaturesEXT MaintenanceFeatures;
				Features2.setPNext(&MaintenanceFeatures);
				Gpu.getFeatures2(&Features2);
				Candidate.Input.bSwapchainMaintenanceFeature = MaintenanceFeatures.swapchainMaintenance1 == vk::True;
			}
			Candidate.Input.bHasSwapchainMaintenanceInstanceDependencies =
				IsInstanceExtensionEnabled(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)
				&& IsInstanceExtensionEnabled(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
			const std::vector<vk::QueueFamilyProperties> QueueFamilies = Gpu.getQueueFamilyProperties();
			for (uint32 QueueIndex = 0; QueueIndex < QueueFamilies.size(); ++QueueIndex)
			{
				FVulkanQueueFamilyCandidate& Queue = Candidate.Input.QueueFamilies.emplace_back();
				Queue.Flags = QueueFamilies[QueueIndex].queueFlags;
				Queue.QueueCount = QueueFamilies[QueueIndex].queueCount;
#ifdef _WIN32
				Queue.bSupportsWin32Presentation = Gpu.getWin32PresentationSupportKHR(QueueIndex);
#endif
			}
			Candidate.Evaluation = EvaluateVulkanPhysicalDeviceCandidate(Candidate.Input);
			Candidates.push_back(std::move(Candidate));
		}

		std::vector<FNativeCandidate*> SuitableCandidates;
		for (FNativeCandidate& Candidate : Candidates)
			if (Candidate.Evaluation.IsSuitable()) SuitableCandidates.push_back(&Candidate);
		if (SuitableCandidates.empty())
		{
			std::vector<FVulkanPhysicalDeviceCandidateInput> Inputs;
			std::vector<FVulkanPhysicalDeviceCandidateEvaluation> Evaluations;
			Inputs.reserve(Candidates.size());
			Evaluations.reserve(Candidates.size());
			for (const FNativeCandidate& Candidate : Candidates)
			{
				Inputs.push_back(Candidate.Input);
				Evaluations.push_back(Candidate.Evaluation);
			}
			throw std::runtime_error(FormatVulkanPhysicalDeviceRejectionDiagnostic(Inputs, Evaluations));
		}
		std::ranges::sort(SuitableCandidates, [](const FNativeCandidate* Left, const FNativeCandidate* Right) {
			return IsVulkanPhysicalDeviceCandidatePreferred(Left->Input, Right->Input);
		});
		FNativeCandidate& Selected = *SuitableCandidates.front();
		auto DeviceCandidate = std::make_unique<FVulkanDevice>(
			this, Selected.Gpu, std::move(Selected.Evaluation));
		DeviceCandidate->InitGpu(static_cast<uint32>(InstanceExtensions.size()));
		Device = DeviceCandidate.release();
	}

}
