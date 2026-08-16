#include "VulkanDynamicRHI.h"
#include "VulkanPresentationSupport.h"

#include "VulkanContext.h"
#include "VulkanGenericPlatform.h"
#include "VulkanCompletion.h"
#include "VulkanExtensions.h"
#include "VulkanDevice.h"
#include "VulkanGPUTiming.h"
#include "VulkanDiagnostics.h"
#include "VulkanSubmission.h"
#include "VulkanCommandBuffer.h"
#include "VulkanBuffer.h"
#include "VulkanRHIPrivate.h"
#include "VulkanViewCache.h"

#include "VulkanDescriptorSets.h"
#include "Misc/Version.h"

// Define the default dispatch loader storage for Vulkan-Hpp. This will allow us to load Vulkan functions at runtime.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace Durin::VulkanRHI
{
	FVulkanDynamicRHI* GVulkanRHI = nullptr;

	IMPLEMENT_MODULE(FVulkanDynamicRHIModule, VulkanRHI)

	FVulkanDynamicRHI::FVulkanDynamicRHI()
		: ViewCache(std::make_unique<FVulkanViewCache>())
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

	auto FVulkanDynamicRHI::SetInitializationPresentationWindow(
		void* InWindowHandle) -> void
	{
		InitializationPresentationWindowHandle = InWindowHandle;
	}

	auto FVulkanDynamicRHI::Init() -> void
	{
		CheckVulkanRHIThread();
		CreateInstance();
		VULKAN_HPP_DEFAULT_DISPATCHER.init(Instance);
		CreateDebugMessenger();
		DebugUtils.SetExtensionActive(
			DiagnosticAvailability.bDebugUtilsActive);
#ifdef __APPLE__
		if (!InitializationPresentationWindowHandle)
			throw std::runtime_error(
				"macOS Vulkan initialization requires the primary native window before device admission.");
		InitializationPresentationSurface = FVulkanGenericPlatform::CreateSurface(
			InitializationPresentationWindowHandle, Instance);
		if (!InitializationPresentationSurface)
			throw std::runtime_error(
				"macOS Vulkan initialization failed to create the primary Metal presentation surface.");
#endif
		SelectDevice(InitializationPresentationSurface);
		DebugUtils.NameObject(Instance, "Durin.Instance");
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
			ERHITextureDimensionFlags::Texture2D
			| ERHITextureDimensionFlags::Texture2DArray
			| ERHITextureDimensionFlags::TextureCube;
		CapabilityCandidate.MaxTextureDimension2D = Limits.maxImageDimension2D;
		CapabilityCandidate.MaxTextureDimensionCube = Limits.maxImageDimensionCube;
		CapabilityCandidate.MaxTextureArrayLayers = Limits.maxImageArrayLayers;
		CapabilityCandidate.ColorSampleCounts = ToRHISampleCounts(
			Limits.framebufferColorSampleCounts & Limits.sampledImageColorSampleCounts);
		CapabilityCandidate.DepthSampleCounts = ToRHISampleCounts(
			Limits.framebufferDepthSampleCounts & Limits.sampledImageDepthSampleCounts);
		CapabilityCandidate.MaxColorAttachments = Limits.maxColorAttachments;
		CapabilityCandidate.MinStorageBufferOffsetAlignment = static_cast<uint32>(
			std::max<vk::DeviceSize>(16, Limits.minStorageBufferOffsetAlignment));
		CapabilityCandidate.MaxStorageBufferRange = Limits.maxStorageBufferRange;
		CapabilityCandidate.MaxComputeWorkGroupCount = {
			Limits.maxComputeWorkGroupCount[0],
			Limits.maxComputeWorkGroupCount[1],
			Limits.maxComputeWorkGroupCount[2]};
		const vk::PhysicalDeviceFeatures Features = Device->GetGpu().getFeatures();
		CapabilityCandidate.bSupportsNonSolidFill = Features.fillModeNonSolid == vk::True;
		CapabilityCandidate.bSupportsDepthClamp = Features.depthClamp == vk::True;
		CapabilityCandidate.bSupportsWideLines = Features.wideLines == vk::True;
		CapabilityCandidate.bSupportsSynchronization2 = Device->SupportsSynchronization2();
		const uint32 GraphicsFamily =
			Device->GetGraphicsQueue()->GetFamilyIndex();
		const uint32 TimestampValidBits =
			Device->GetQueueFamilyProperties(GraphicsFamily).timestampValidBits;
		const double TimestampPeriod = static_cast<double>(Limits.timestampPeriod);
		CapabilityCandidate.bSupportsGPUTimestamps = TimestampValidBits != 0
			&& std::isfinite(TimestampPeriod) && TimestampPeriod > 0.0;
		CapabilityCandidate.GPUTimestampNanosecondsPerTick =
			CapabilityCandidate.bSupportsGPUTimestamps ? TimestampPeriod : 0.0;
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
		ViewCache->Clear();
		if (const char* CaptureBaseline =
			std::getenv("DURIN_VULKAN_MEMORY_BASELINE");
			CaptureBaseline && std::string_view(CaptureBaseline) == "1")
		{
			DURIN_INFO("Vulkan M4 baseline: {}",
				FormatVulkanMemoryBaselineStatistics(
					GetVulkanMemoryBaselineStatistics()));
		}
		// Render thread should already be stopped at this point.
		delete Device;
		Device = nullptr;
		if (InitializationPresentationSurface)
		{
			Instance.destroySurfaceKHR(InitializationPresentationSurface);
			InitializationPresentationSurface = VK_NULL_HANDLE;
		}
		InitializationPresentationWindowHandle = nullptr;
		DebugUtils.ResetDevice();
		DestroyDebugMessenger();
		if (Instance)
		{
			Instance.destroy();
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			RecordVulkanInstanceDestroyedForTest();
#endif
			Instance = nullptr;
		}
		InstanceExtensions.clear();
		InstanceLayers.clear();
		DiagnosticAvailability = {};
	}

	auto FVulkanDynamicRHI::RHIBeginFrame(
		const FRHIBeginFrameArgs& Args) -> void
	{
		CheckVulkanRHIThread();
		ViewCache->Trim(Args.FrameNumber);
		const uint32 FrameIndex = static_cast<uint32>(
			Args.FrameNumber % kFrameInFlight);
		GVulkanMemoryBaselineTracker.BeginFrame();
		Device->GetCompletionTracker().Poll();
		Device->GetGPUTimingManager().Poll();
		Device->SetCurrentFrameIndex(FrameIndex);
		FVulkanFrame& Frame = Device->GetCurrentFrame();
		Frame.Prepare();
		Device->GetGlobalDescriptorPool().PrepareForUse();
		Device->GetImmediateContext()->RHIBeginFrame(Args);
	}

	auto FVulkanDynamicRHI::RHICreateGPUTimingQuery()
		-> TRefCountPtr<FRHIGPUTimingQuery>
	{
		if (!RHIGetCapabilities()->bSupportsGPUTimestamps) return nullptr;
		TRefCountPtr<FRHIGPUTimingQuery> Result;
		if (IsInRHIThread())
			return Device->GetGPUTimingManager().CreateQuery();
		GCommandListExecutor.ExecuteSynchronousOperation(false, [this, &Result]() {
			Result = Device->GetGPUTimingManager().CreateQuery();
		});
		return Result;
	}

	auto FVulkanDynamicRHI::RHIGetGPUTimingResult(
		const FRHIGPUTimingQuery* Query) const -> FRHIGPUTimingResult
	{
		return Query ? Query->GetResult() : FRHIGPUTimingResult{};
	}

	auto FVulkanDynamicRHI::RHIBeginFrame_RenderThread(
		FRHICommandListImmediate& RHICmdList) -> void
	{
		check(!GRHIThread || IsInRenderingThread());
		FDynamicRHI::RHIBeginFrame_RenderThread(RHICmdList);
		auto& Allocator = Device->GetDynamicUniformBufferAllocator();
		GCommandListExecutor.ExecuteSynchronousOperation(true,
			[&Allocator]() { Allocator.PrepareForProducer(); });
		Device->GetDynamicStorageBufferAllocator().BeginFrameProducer(
			static_cast<uint32>(
				GCommandListExecutor.GetFrameNumber() % kFrameInFlight));
	}

	auto FVulkanDynamicRHI::RHIEndFrame() -> void
	{
		CheckVulkanRHIThread();
		Device->GetImmediateContext()->RHIEndFrame();
		const FVulkanCompletionToken Token =
			Device->GetCompletionTracker().GetLastSubmittedToken();
		Device->GetGlobalDescriptorPool().RetireUsedPools(Token);
		Device->GetDynamicUniformBufferAllocator().RetireProducer(Token);
		Device->GetCompletionTracker().Poll();
		Device->GetDeferredDeletionQueue().ReleaseResources();
	}

	auto FVulkanDynamicRHI::RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void
	{
		FDynamicRHI::RHIEndFrame_RenderThread(RHICmdList);
	}

	auto FVulkanDynamicRHI::RHIGetMemoryStatistics() const
		-> FRHIMemoryStatistics
	{
		return GetRHIMemoryStatistics();
	}

	auto FVulkanDynamicRHI::RHIResetMemoryStatistics() -> void
	{
		ResetVulkanMemoryBaselineStatistics();
	}

	auto FVulkanDynamicRHI::RHIGetDiagnosticSnapshot() const
		-> FRHIDiagnosticSnapshot
	{
		CheckVulkanRHIThread();
		FRHIDiagnosticSnapshot Result;
		Result.Executor = GCommandListExecutor.GetStats();
		Result.Availability = {
				.bRequested = DiagnosticAvailability.bRequested,
				.bDebugUtilsSupported = DiagnosticAvailability.bDebugUtilsSupported,
				.bDebugUtilsActive = DiagnosticAvailability.bDebugUtilsActive,
				.bValidationLayerSupported = DiagnosticAvailability.bValidationLayerSupported,
				.bValidationLayerActive = DiagnosticAvailability.bValidationLayerActive,
				.bMessengerActive = DiagnosticAvailability.bMessengerActive,
		};
		if (!Device) return Result;
		Result.GraphicsCache = Device->GetGraphicsCacheStatistics();
		Result.Memory = GetRHIMemoryStatistics();
		const auto& Completion = Device->GetCompletionTracker();
		Result.Completion = {
				.LastSubmittedToken = Completion.GetLastSubmittedToken(),
				.CompletedToken = Completion.GetCompletedToken(),
				.PendingSubmissions = Completion.GetPendingSubmissionCount(),
				.RetirementPendingCount = Result.Memory.RetirementPendingCount,
				.RetirementHighWater = Result.Memory.RetirementHighWater,
				.RetirementReleasedCount = Result.Memory.RetirementReleasedCount,
				.RetirementMaxTokenLag = Result.Memory.RetirementMaxTokenLag,
		};
		const FVulkanDebugMessageStatistics Messages =
			DebugCallbackState.Snapshot();
		Result.Messages = {
				.Total = Messages.TotalCount,
				.Error = Messages.ErrorCount,
				.Warning = Messages.WarningCount,
				.Information = Messages.InformationCount,
				.Verbose = Messages.VerboseCount,
				.General = Messages.GeneralCount,
				.Validation = Messages.ValidationCount,
				.Performance = Messages.PerformanceCount,
				.Truncation = Messages.TruncatedCount,
				.RecursionDrop = Messages.RecursionDropCount,
		};
		const FVulkanDebugUtilsStatistics Naming = DebugUtils.Snapshot();
		Result.Naming = {
				.NamingAttempts = Naming.NamingAttemptCount,
				.NamingFailures = Naming.NamingFailureCount,
				.NamingUnavailableSkips = Naming.NamingUnavailableSkipCount,
				.LabelBegins = Naming.LabelBeginCount,
				.LabelEnds = Naming.LabelEndCount,
				.LabelUnavailableSkips = Naming.LabelUnavailableSkipCount,
				.InvalidRegionCount =
					FRHICommandListBase::GetInvalidDiagnosticRegionCount(),
				.ActiveRegionDepth = Naming.ActiveLabelDepth,
				.RegionHighWater = Naming.LabelHighWater,
		};
		const FVulkanGPUTimingStatistics Timing =
			Device->GetGPUTimingManager().Snapshot();
		Result.Timing = {
				.IntervalCapacity = Timing.IntervalCapacity,
				.AllocatedPages = Timing.AllocatedPages,
				.LiveIntervals = Timing.LiveIntervals,
				.PendingIntervals = Timing.PendingIntervals,
				.ReadyIntervals = Timing.ReadyIntervals,
				.IntervalHighWater = Timing.IntervalHighWater,
				.ExhaustionCount = Timing.ExhaustionCount,
				.AllocationFailureCount = Timing.AllocationFailureCount,
				.ReuseCount = Timing.ReuseCount,
				.InvalidRecordingCount = Timing.InvalidRecordingCount,
				.ResultPollCount = Timing.ResultPollCount,
				.ReadyResultCount = Timing.ReadyResultCount,
				.ConversionOverflowCount = Timing.ConversionOverflowCount,
		};
		return Result;
	}

	auto FVulkanDynamicRHI::RHIResetDiagnosticStatistics() -> void
	{
		CheckVulkanRHIThread();
		if (Device)
		{
			Device->ResetGraphicsCacheStatistics();
			ResetVulkanMemoryBaselineStatistics();
			Device->GetGPUTimingManager().ResetStatistics();
		}
		DebugCallbackState.Reset();
		DebugUtils.ResetStatistics();
		FRHICommandListBase::ResetInvalidDiagnosticRegionCount();
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
		auto& Allocator = Device->GetDynamicUniformBufferAllocator();
		FRHIUniformBufferRange Result;
		if (Allocator.TryAllocate(Data, Size, Result))
		{
			return Result;
		}

		GCommandListExecutor.ExecuteSynchronousOperation(true,
			[&Allocator, Size]() {
				Allocator.ReservePage(Size);
			});
		requiref(Allocator.TryAllocate(Data, Size, Result),
			"A prepared dynamic-uniform overflow page must satisfy the pending allocation.");
		return Result;
	}

	auto FVulkanDynamicRHI::RHIAllocateDynamicStorageBuffer(
		FRHICommandListImmediate&,
		const void* Data,
		uint32 Size) -> FRHIStorageBufferRange
	{
		if (!Data || Size == 0
			|| Size > Device->GetGpuProperties().limits.maxStorageBufferRange
			|| Size > FVulkanDynamicStorageBufferAllocator::MaximumBytesPerFrame)
			return {};
		const uint32 FrameIndex = static_cast<uint32>(
			GCommandListExecutor.GetFrameNumber() % kFrameInFlight);
		auto& Allocator = Device->GetDynamicStorageBufferAllocator();
		FRHIStorageBufferRange Result;
		if (Allocator.TryAllocate(FrameIndex, Data, Size, Result)) return Result;
		GCommandListExecutor.ExecuteSynchronousOperation(true,
			[&Allocator, FrameIndex, Size]() { Allocator.ReservePage(FrameIndex, Size); });
		if (!Allocator.TryAllocate(FrameIndex, Data, Size, Result)) return {};
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
		FVulkanInstanceExtensionRequestInput ExtensionRequestInput;
#ifdef _WIN32
		ExtensionRequestInput.SurfaceProviderRequiredExtensions.emplace_back(
			VK_KHR_SURFACE_EXTENSION_NAME);
		ExtensionRequestInput.SurfaceProviderRequiredExtensions.emplace_back(
			VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
		for (const char* RequiredExtension : GMonaRequiredVulkanInstanceExtensions)
			ExtensionRequestInput.SurfaceProviderRequiredExtensions.emplace_back(
				RequiredExtension);
#ifdef __APPLE__
		ExtensionRequestInput.bRequirePortabilityEnumeration = true;
		ExtensionRequestInput.SurfaceProviderRequiredExtensions.emplace_back(
			VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
#endif
		const FVulkanInstanceExtensionRequest ExtensionRequest =
			BuildVulkanInstanceExtensionRequest(ExtensionRequestInput);
		if (!ExtensionRequest.IsSuccess())
			throw std::runtime_error(ExtensionRequest.Diagnostic);
		NegotiationInput.PlatformRequiredExtensions = ExtensionRequest.RequiredExtensions;
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
		// Metal argument buffers currently produce unstable descriptor reads for
		// Durin's per-frame suballocated uniform buffers. Select MoltenVK's
		// discrete resource-index path at instance creation so every descriptor
		// set follows the same qualified binding model.
		const VkBool32 bUseMetalArgumentBuffers = VK_FALSE;
		const vk::LayerSettingEXT MetalArgumentBufferSetting(
			"MoltenVK",
			"MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS",
			vk::LayerSettingTypeEXT::eBool32,
			1,
			&bUseMetalArgumentBuffers);
		const vk::LayerSettingsCreateInfoEXT MoltenVKLayerSettings(
			1, &MetalArgumentBufferSetting);
		InstanceInfo.setPNext(&MoltenVKLayerSettings);
#endif

		if (ExtensionRequest.bEnablePortabilityEnumeration)
			InstanceInfo.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
		FVulkanDiagnosticAvailability AvailabilityCandidate;
		AvailabilityCandidate.bRequested = ValidationPolicy.bRequestDiagnostics;
		AvailabilityCandidate.bDebugUtilsSupported = std::ranges::find(
			NegotiationInput.AvailableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
			!= NegotiationInput.AvailableExtensions.end();
		AvailabilityCandidate.bDebugUtilsActive = std::ranges::find(
			Negotiation.EnabledExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
			!= Negotiation.EnabledExtensions.end();
		AvailabilityCandidate.bValidationLayerSupported = std::ranges::find(
			NegotiationInput.AvailableLayers, "VK_LAYER_KHRONOS_validation")
			!= NegotiationInput.AvailableLayers.end();
		AvailabilityCandidate.bValidationLayerActive = std::ranges::find(
			Negotiation.EnabledLayers, "VK_LAYER_KHRONOS_validation")
			!= Negotiation.EnabledLayers.end();
		try
		{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Instance);
#endif
			vk::Instance InstanceCandidate = vk::createInstance(InstanceInfo);
			InstanceExtensions = std::move(Negotiation.EnabledExtensions);
			InstanceLayers = std::move(Negotiation.EnabledLayers);
			DiagnosticAvailability = AvailabilityCandidate;
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

	auto FVulkanDynamicRHI::CreateDebugMessenger() -> void
	{
		check(!DebugMessenger);
		if (!DiagnosticAvailability.bDebugUtilsActive) return;

		vk::DebugUtilsMessengerCreateInfoEXT CreateInfo;
		CreateInfo.setMessageSeverity(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose
			| vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo
			| vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning
			| vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
			.setMessageType(vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral
				| vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation
				| vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance)
			.setPfnUserCallback(&VulkanDebugUtilsCallback)
			.setPUserData(&DebugCallbackState);
		try
		{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(
				EVulkanCreateFailurePoint::DebugMessenger);
#endif
			DebugMessenger = Instance.createDebugUtilsMessengerEXT(CreateInfo);
			DiagnosticAvailability.bMessengerActive = true;
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			RecordVulkanDebugMessengerCreatedForTest();
#endif
		}
		catch (const std::exception& Exception)
		{
			DURIN_WARN(
				"Optional Vulkan debug messenger creation failed; startup continues without callback diagnostics: {}",
				Exception.what());
		}
	}

	auto FVulkanDynamicRHI::DestroyDebugMessenger() -> void
	{
		if (!DebugMessenger) return;
		Instance.destroyDebugUtilsMessengerEXT(DebugMessenger);
		DebugMessenger = nullptr;
		DiagnosticAvailability.bMessengerActive = false;
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		RecordVulkanDebugMessengerDestroyedForTest();
#endif
	}

	auto FVulkanDynamicRHI::SelectDevice(
		vk::SurfaceKHR PresentationSurface) -> void
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
			Candidate.Input.MaxComputeWorkGroupCount = {
				Properties.limits.maxComputeWorkGroupCount[0],
				Properties.limits.maxComputeWorkGroupCount[1],
				Properties.limits.maxComputeWorkGroupCount[2]};
			Candidate.Input.bFillModeNonSolid = Features.fillModeNonSolid == vk::True;
			Candidate.Input.bIndependentBlend = Features.independentBlend == vk::True;
#ifdef __APPLE__
			Candidate.Input.bRequirePortabilitySubset = true;
#endif
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
				Queue.bSupportsPresentation =
					QueryNativeVulkanPresentationSupport(
						Gpu, QueueIndex, PresentationSurface);
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

	auto FVulkanDynamicRHI::TakeInitializationPresentationSurface(
		void* WindowHandle) const -> vk::SurfaceKHR
	{
		if (WindowHandle != InitializationPresentationWindowHandle)
			return VK_NULL_HANDLE;
		return std::exchange(
			InitializationPresentationSurface, VK_NULL_HANDLE);
	}

}
