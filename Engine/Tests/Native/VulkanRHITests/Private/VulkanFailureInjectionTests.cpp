#include <gtest/gtest.h>

#include "Modules/ModuleManager.h"
#include "Logging/Logger.h"
#include "PCH.VulkanRHI.h"
#include "CoreGlobals.h"
#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RHIGlobals.h"
#include "RHICommandList.h"
#include <future>
#include "RenderingThread.h"
#include "SlangShaderCompiler.h"
#include "VulkanRHIPrivate.h"
#include "VulkanCreationTiming.h"
#include "VulkanCreation.h"
#include "VulkanDynamicRHI.h"
#include "VulkanExtensions.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanBuffer.h"
#include "VulkanDiagnostics.h"
#include "VulkanGPUTiming.h"
#include "VulkanContext.h"
#include "VulkanSwapchain.h"
#include "VulkanViewport.h"
#include "VulkanRHITestEnvironment.h"
#include "Window/GenericWindow.h"
#include "Window/GenericWindowDefinition.h"

namespace Durin::VulkanRHI
{
	namespace
	{
#ifdef _WIN32
		constexpr std::string_view PlatformSurfaceExtension =
			VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#else
		constexpr std::string_view PlatformSurfaceExtension =
			VK_EXT_METAL_SURFACE_EXTENSION_NAME;
#endif

		auto ExpectVulkanModuleUnloaded() -> void
		{
			if (FModuleManager::Get().IsModuleLoaded("VulkanRHI"))
				EXPECT_TRUE(FModuleManager::Get().UnloadModule("VulkanRHI"));
		}

		struct FDebugMessageCapture
		{
			EVulkanDebugMessageSeverity Severity =
				EVulkanDebugMessageSeverity::Information;
			bool bGeneral = false;
			bool bValidation = false;
			bool bPerformance = false;
			bool bTruncated = false;
			std::string Message;
		};

		auto CaptureDebugMessage(
			const FVulkanClassifiedDebugMessage& Message, void* UserData) -> void
		{
			auto& Capture = *static_cast<FDebugMessageCapture*>(UserData);
			Capture = {
				.Severity = Message.Severity,
				.bGeneral = Message.bGeneral,
				.bValidation = Message.bValidation,
				.bPerformance = Message.bPerformance,
				.bTruncated = Message.bTruncated,
				.Message = std::string(Message.Message),
			};
		}

		struct FRecursiveDebugMessageContext
		{
			FVulkanDebugCallbackState* State = nullptr;
			uint32 SinkCallCount = 0;
		};

		auto ReenterDebugMessage(
			const FVulkanClassifiedDebugMessage&, void* UserData) -> void
		{
			auto& Context = *static_cast<FRecursiveDebugMessageContext*>(UserData);
			++Context.SinkCallCount;
			Context.State->HandleMessage(
				vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo,
				vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral,
				"recursive", &ReenterDebugMessage, &Context);
		}

		auto MakeInstanceNegotiationInput() -> FVulkanInstanceNegotiationInput
		{
			FVulkanInstanceNegotiationInput Input;
			Input.LoaderApiVersion = VK_API_VERSION_1_3;
			Input.PlatformRequiredExtensions = {
				VK_KHR_SURFACE_EXTENSION_NAME,
				std::string(PlatformSurfaceExtension)};
			Input.AvailableExtensions = Input.PlatformRequiredExtensions;
			return Input;
		}

		auto MakePhysicalDeviceCandidateInput() -> FVulkanPhysicalDeviceCandidateInput
		{
			FVulkanPhysicalDeviceCandidateInput Input;
			Input.DeviceName = "Suitable GPU";
			Input.DeviceType = vk::PhysicalDeviceType::eDiscreteGpu;
			Input.ApiVersion = VK_API_VERSION_1_3;
			Input.VendorId = 1;
			Input.DeviceId = 2;
			Input.MaxImageDimension2D = 8192;
			Input.MaxImageDimensionCube = 8192;
			Input.MaxImageArrayLayers = 256;
			Input.MaxComputeWorkGroupCount = {65535, 65535, 65535};
			Input.bFillModeNonSolid = true;
			Input.bIndependentBlend = true;
			Input.bShaderDrawParameters = true;
			Input.AvailableExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
			Input.QueueFamilies = {
				{vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute
					| vk::QueueFlagBits::eTransfer, 1, true}};
			return Input;
		}

		class FVulkanCreateFailureInjectionTests : public testing::Test
		{
		protected:
			auto SetUp() -> void override
			{
				if (const char* ExistingMode = std::getenv("DURIN_RHI_EXECUTION"))
				{
					PreviousExecutionMode = ExistingMode;
				}
				if (const char* ExistingValidation =
					std::getenv("DURIN_VULKAN_VALIDATION"))
				{
					PreviousValidationMode = ExistingValidation;
				}
				if (const char* Existing = std::getenv("DURIN_VULKAN_FULL_DESCRIPTOR_VALIDATION"))
					PreviousDescriptorValidationMode = Existing;
				_putenv_s("DURIN_VULKAN_FULL_DESCRIPTOR_VALIDATION", "off");
				_putenv_s("DURIN_RHI_EXECUTION", "threaded");
				ResetVulkanCreateFailures();
			}

			auto TearDown() -> void override
			{
				SetVulkanPipelineCompilationHookForTest({});
				if (GDynamicRHI)
				{
					RHIExit();
				}
				ExpectVulkanModuleUnloaded();
				ResetVulkanCreateFailures();
				_putenv_s("DURIN_RHI_EXECUTION",
					PreviousExecutionMode ? PreviousExecutionMode->c_str() : "");
				_putenv_s("DURIN_VULKAN_VALIDATION",
					PreviousValidationMode ? PreviousValidationMode->c_str() : "");
				_putenv_s("DURIN_VULKAN_FULL_DESCRIPTOR_VALIDATION",
					PreviousDescriptorValidationMode ? PreviousDescriptorValidationMode->c_str() : "");
			}

			std::optional<std::string> PreviousExecutionMode;
			std::optional<std::string> PreviousValidationMode;
			std::optional<std::string> PreviousDescriptorValidationMode;
		};
	}

	class FVulkanPublicRHIConformanceTests
		: public FVulkanCreateFailureInjectionTests
	{
	};

	TEST_F(FVulkanCreateFailureInjectionTests, RecoveryFactorySeparatesValuesErrorsAndDiagnostics)
	{
		auto& Logger = FLogger::Get();
		ASSERT_TRUE(Logger.Initialize({}));
		struct FLoggerScope { ~FLoggerScope() { FLogger::Get().Shutdown(); } } LoggerScope;
		Logger.Flush();
		const auto Cursor = Logger.ReadRecords(1, 0).NewestAvailableSequence + 1;
		auto Fail = []() -> std::shared_ptr<int> {
			throw vk::OutOfDeviceMemoryError("expected allocation failure");
		};
		const auto Failed = TryCreateVulkanResource(Fail);
		ASSERT_FALSE(Failed);
		EXPECT_EQ(Failed.error().Failure, ERHIResourceCreationFailure::OutOfMemory);
		EXPECT_EQ(Failed.error().Source, ERHICreationFailureSource::NativeBackend);
		EXPECT_EQ(Failed.error().NativeCode, static_cast<int32>(vk::Result::eErrorOutOfDeviceMemory));
		const auto Resource = TryCreateVulkanResource([] { return std::make_shared<int>(42); });
		ASSERT_TRUE(Resource);
		ASSERT_TRUE(*Resource);
		EXPECT_EQ(**Resource, 42);
		const auto Null = TryCreateVulkanResource([] { return std::shared_ptr<int>{}; });
		ASSERT_FALSE(Null);
		EXPECT_EQ(Null.error().Source, ERHICreationFailureSource::BackendReturnedNull);
		Logger.Flush();
		EXPECT_TRUE(Logger.ReadRecords(Cursor).Records.empty());

		EXPECT_FALSE(CreateVulkanResource(Fail, "test resource"));
		Logger.Flush();
		const auto Logs = Logger.ReadRecords(Cursor);
		ASSERT_EQ(Logs.Records.size(), 1u);
		EXPECT_EQ(Logs.Records.front().Level, ELogLevel::Error);
	}

	TEST_F(FVulkanCreateFailureInjectionTests, NullableFactoryPreservesTerminalExceptions)
	{
		EXPECT_THROW(CreateVulkanResource([]() -> std::shared_ptr<int> {
			throw vk::DeviceLostError("terminal device loss");
		}, "test resource"), vk::DeviceLostError);
		EXPECT_THROW(CreateVulkanResource([]() -> std::shared_ptr<int> {
			throw std::logic_error("internal invariant failure");
		}, "test resource"), std::logic_error);
		EXPECT_THROW(TryCreateVulkanResource([]() -> std::shared_ptr<int> {
			throw vk::DeviceLostError("terminal device loss");
		}), vk::DeviceLostError);
		EXPECT_THROW(TryCreateVulkanResource([]() -> std::shared_ptr<int> {
			throw std::logic_error("internal invariant failure");
		}), std::logic_error);
	}

	TEST(FVulkanDebugUtilsTests, UnavailableNamingIsCountedAndNonFatal)
	{
		FVulkanDebugUtils DebugUtils;
		DebugUtils.NameObject(
			vk::Buffer(reinterpret_cast<VkBuffer>(uintptr_t{1})), "UnavailableBuffer");
		const FVulkanDebugUtilsStatistics Statistics = DebugUtils.Snapshot();
		EXPECT_EQ(Statistics.NamingAttemptCount, 1u);
		EXPECT_EQ(Statistics.NamingUnavailableSkipCount, 1u);
		EXPECT_EQ(Statistics.NamingFailureCount, 0u);
	}

	TEST(FVulkanGPUTimingTests, ConvertsMaskedWrappedAndFractionalDurations)
	{
		bool bOverflow = false;
		EXPECT_EQ(ConvertVulkanTimestampDuration(
			100, 125, 32, 1.0, bOverflow), 25u);
		EXPECT_FALSE(bOverflow);
		EXPECT_EQ(ConvertVulkanTimestampDuration(
			0xfffffff0u, 0x10u, 32, 1.0, bOverflow), 32u);
		EXPECT_FALSE(bOverflow);
		EXPECT_EQ(ConvertVulkanTimestampDuration(
			3, 8, 36, 52.083333, bOverflow), 260u);
		EXPECT_FALSE(bOverflow);
		EXPECT_EQ(ConvertVulkanTimestampDuration(
			0, std::numeric_limits<uint64>::max(), 64, 2.0, bOverflow),
			std::numeric_limits<uint64>::max());
		EXPECT_TRUE(bOverflow);
		EXPECT_EQ(ConvertVulkanTimestampDuration(0, 1, 0, 1.0, bOverflow), 0u);
		EXPECT_EQ(ConvertVulkanTimestampDuration(0, 1, 64, 0.0, bOverflow), 0u);
	}

	TEST(FVulkanDebugCallbackTests,
		ClassifiesBoundsCountsAndRejectsRecursiveDelivery)
	{
		FVulkanDebugCallbackState State;
		FDebugMessageCapture Capture;
		State.HandleMessage(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning,
			vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation
				| vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
			"validation warning", &CaptureDebugMessage, &Capture);
		EXPECT_EQ(Capture.Severity, EVulkanDebugMessageSeverity::Warning);
		EXPECT_FALSE(Capture.bGeneral);
		EXPECT_TRUE(Capture.bValidation);
		EXPECT_TRUE(Capture.bPerformance);
		EXPECT_EQ(Capture.Message, "validation warning");

		std::string Oversized(
			FVulkanClassifiedDebugMessage::MaximumMessageBytes + 1, 'x');
		State.HandleMessage(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
			{}, Oversized.c_str(), &CaptureDebugMessage, &Capture);
		EXPECT_EQ(Capture.Severity, EVulkanDebugMessageSeverity::Error);
		EXPECT_TRUE(Capture.bGeneral);
		EXPECT_TRUE(Capture.bTruncated);
		EXPECT_EQ(Capture.Message.size(),
			FVulkanClassifiedDebugMessage::MaximumMessageBytes);

		State.HandleMessage(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose,
			vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral, "verbose");
		State.HandleMessage(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo,
			vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral, "info");
		FRecursiveDebugMessageContext Recursive{.State = &State};
		State.HandleMessage(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo,
			vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral,
			"outer", &ReenterDebugMessage, &Recursive);

		const FVulkanDebugMessageStatistics Statistics = State.Snapshot();
		EXPECT_EQ(Statistics.TotalCount, 5u);
		EXPECT_EQ(Statistics.VerboseCount, 1u);
		EXPECT_EQ(Statistics.InformationCount, 2u);
		EXPECT_EQ(Statistics.WarningCount, 1u);
		EXPECT_EQ(Statistics.ErrorCount, 1u);
		EXPECT_EQ(Statistics.GeneralCount, 4u);
		EXPECT_EQ(Statistics.ValidationCount, 1u);
		EXPECT_EQ(Statistics.PerformanceCount, 1u);
		EXPECT_EQ(Statistics.TruncatedCount, 1u);
		EXPECT_EQ(Statistics.RecursionDropCount, 1u);
		EXPECT_EQ(Recursive.SinkCallCount, 1u);

		State.Reset();
		EXPECT_EQ(State.Snapshot().TotalCount, 0u);
	}

	TEST(FVulkanInstanceNegotiationTests, ValidationPolicyIsConfigurationAware)
	{
		EXPECT_TRUE(ResolveVulkanValidationPolicy(nullptr, true, false).bRequestDiagnostics);
		EXPECT_FALSE(ResolveVulkanValidationPolicy("auto", false, false).bRequestDiagnostics);
		EXPECT_TRUE(ResolveVulkanValidationPolicy("on", false, false).bRequestDiagnostics);
		EXPECT_FALSE(ResolveVulkanValidationPolicy("off", true, false).bRequestDiagnostics);
		EXPECT_FALSE(ResolveVulkanValidationPolicy("on", true, true).bRequestDiagnostics);
		const FVulkanValidationPolicy Invalid = ResolveVulkanValidationPolicy("invalid", true, false);
		EXPECT_TRUE(Invalid.bInvalidSetting);
		EXPECT_TRUE(Invalid.bRequestDiagnostics);
	}

	TEST(FVulkanInstanceNegotiationTests,
		BuildsDeterministicSurfaceAndPortabilityRequirements)
	{
		FVulkanInstanceExtensionRequestInput Input;
		const auto Headless = BuildVulkanInstanceExtensionRequest(Input);
		EXPECT_EQ(Headless.RequiredExtensions,
			(std::vector<std::string>{VK_KHR_SURFACE_EXTENSION_NAME}));

		Input.SurfaceProviderRequiredExtensions = {
			VK_KHR_SURFACE_EXTENSION_NAME,
			VK_KHR_SURFACE_EXTENSION_NAME,
			"VK_EXT_provider_surface"};
		Input.bRequirePortabilityEnumeration = true;
		const FVulkanInstanceExtensionRequest Result =
			BuildVulkanInstanceExtensionRequest(Input);
		EXPECT_EQ(Result.RequiredExtensions,
			(std::vector<std::string>{
				VK_KHR_SURFACE_EXTENSION_NAME,
				"VK_EXT_provider_surface",
				VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME}));
		EXPECT_TRUE(Result.bEnablePortabilityEnumeration);
	}

	TEST(FVulkanInstanceNegotiationTests, RejectsLoaderFloorAndMissingPlatformRequirement)
	{
		FVulkanInstanceNegotiationInput Input = MakeInstanceNegotiationInput();
		Input.LoaderApiVersion = VK_API_VERSION_1_0;
		FVulkanInstanceNegotiationResult Result = NegotiateVulkanInstance(Input);
		EXPECT_FALSE(Result.IsSuccess());
		ASSERT_EQ(Result.RejectionReasons.size(), 1u);
		EXPECT_FALSE(Result.RejectionReasons.front().empty());

		Input = MakeInstanceNegotiationInput();
		Input.AvailableExtensions.pop_back();
		Result = NegotiateVulkanInstance(Input);
		EXPECT_FALSE(Result.IsSuccess());
		ASSERT_EQ(Result.RejectionReasons.size(), 1u);
		EXPECT_FALSE(Result.RejectionReasons.front().empty());
	}

	TEST(FVulkanInstanceNegotiationTests, PromotedAndOptionalRequirementsAreDeduplicated)
	{
		FVulkanInstanceNegotiationInput Input = MakeInstanceNegotiationInput();
		Input.PlatformRequiredExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
		Input.AvailableExtensions.insert(Input.AvailableExtensions.end(), {
			VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
			VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
			VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME});
		const FVulkanInstanceNegotiationResult Result = NegotiateVulkanInstance(Input);
		ASSERT_TRUE(Result.IsSuccess()) << ::testing::PrintToString(Result.RejectionReasons);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_SURFACE_EXTENSION_NAME)), 1);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)), 0);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)), 1);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)), 1);
	}

	TEST(FVulkanInstanceNegotiationTests, SurfaceMaintenanceRequiresEnabledSurface)
	{
		FVulkanInstanceNegotiationInput Input = MakeInstanceNegotiationInput();
		Input.PlatformRequiredExtensions.clear();
		Input.AvailableExtensions.insert(Input.AvailableExtensions.end(), {
			VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
			VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME});
		const auto Result = NegotiateVulkanInstance(Input);
		ASSERT_TRUE(Result.IsSuccess()) << ::testing::PrintToString(Result.RejectionReasons);
		EXPECT_TRUE(Result.EnabledExtensions.empty());
	}

	TEST(FVulkanInstanceNegotiationTests, OptionalDiagnosticsActivateIndependently)
	{
		FVulkanInstanceNegotiationInput Input = MakeInstanceNegotiationInput();
		Input.bRequestDiagnostics = true;
		FVulkanInstanceNegotiationResult Result = NegotiateVulkanInstance(Input);
		ASSERT_TRUE(Result.IsSuccess()) << ::testing::PrintToString(Result.RejectionReasons);
		EXPECT_TRUE(Result.EnabledExtensions.empty()
			|| std::ranges::find(Result.EnabledExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
				== Result.EnabledExtensions.end());
		EXPECT_TRUE(Result.EnabledLayers.empty());

		Input.AvailableLayers.emplace_back("VK_LAYER_KHRONOS_validation");
		Result = NegotiateVulkanInstance(Input);
		EXPECT_EQ(Result.EnabledLayers,
			std::vector<std::string>{"VK_LAYER_KHRONOS_validation"});
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)), 0);

		Input.AvailableExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		Result = NegotiateVulkanInstance(Input);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)), 1);
	}

	TEST(FVulkanInstanceNegotiationTests, FailedCandidateDoesNotAffectFollowingSuccess)
	{
		FVulkanInstanceNegotiationInput Input = MakeInstanceNegotiationInput();
		Input.AvailableExtensions.clear();
		EXPECT_FALSE(NegotiateVulkanInstance(Input).IsSuccess());
		Input = MakeInstanceNegotiationInput();
		const FVulkanInstanceNegotiationResult Result = NegotiateVulkanInstance(Input);
		EXPECT_TRUE(Result.IsSuccess()) << ::testing::PrintToString(Result.RejectionReasons);
		EXPECT_EQ(Result.EnabledExtensions.size(), 2u);
	}

	TEST(FVulkanDeviceCandidateTests, RejectsEveryHardRequirementBeforeRanking)
	{
		struct FCase
		{
			const char* Requirement;
			std::function<void(FVulkanPhysicalDeviceCandidateInput&)> BreakRequirement;
		};
		const std::array Cases{
			FCase{"DeviceVersionTooOld", [](auto& Input) { Input.ApiVersion = VK_API_VERSION_1_0; }},
			FCase{"MissingSwapchainExtension", [](auto& Input) { Input.AvailableExtensions.clear(); }},
			FCase{"MissingFillModeNonSolid", [](auto& Input) { Input.bFillModeNonSolid = false; }},
			FCase{"MissingIndependentBlend", [](auto& Input) { Input.bIndependentBlend = false; }},
			FCase{"MissingShaderDrawParameters", [](auto& Input) { Input.bShaderDrawParameters = false; }},
			FCase{"InvalidImageDimension2D", [](auto& Input) { Input.MaxImageDimension2D = 0; }},
			FCase{"InvalidImageDimensionCube", [](auto& Input) { Input.MaxImageDimensionCube = 0; }},
			FCase{"InsufficientArrayLayers", [](auto& Input) { Input.MaxImageArrayLayers = 5; }},
			FCase{"InvalidComputeWorkGroupCount", [](auto& Input) {
				Input.MaxComputeWorkGroupCount[1] = 0;
			}},
			FCase{"MissingPresentationQueue", [](auto& Input) {
				Input.QueueFamilies[0].bSupportsPresentation = false;
			}},
		};
		for (const FCase& Case : Cases)
		{
			SCOPED_TRACE(Case.Requirement);
			FVulkanPhysicalDeviceCandidateInput Input = MakePhysicalDeviceCandidateInput();
			Case.BreakRequirement(Input);
			const FVulkanPhysicalDeviceCandidateEvaluation Result =
				EvaluateVulkanPhysicalDeviceCandidate(Input);
			ASSERT_FALSE(Result.IsSuitable());
			ASSERT_FALSE(Result.RejectionReasons.empty());
			EXPECT_TRUE(std::ranges::all_of(Result.RejectionReasons,
				[](const std::string& Reason) { return !Reason.empty(); }));
		}
	}

	TEST(FVulkanDeviceCandidateTests, ComputeTopologyPreferenceAndForcedFallbackAreDeterministic)
	{
		auto Input = MakePhysicalDeviceCandidateInput();
		Input.ApiVersion = VK_API_VERSION_1_2;
		Input.bTimelineSemaphoreFeature = true;
		Input.QueueFamilies[0].QueueCount = 2;
		Input.QueueFamilies.push_back({vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eTransfer, 1, false});
		Input.ComputeQueuePolicy = EVulkanComputeQueuePolicy::Automatic;
		auto Result = EvaluateVulkanPhysicalDeviceCandidate(Input);
		ASSERT_TRUE(Result.IsSuitable());
		EXPECT_EQ(Result.ComputeQueueFamilyIndex, 1);
		EXPECT_EQ(Result.ComputeQueueIndex, 0u);
		Input.ComputeQueuePolicy = EVulkanComputeQueuePolicy::SameFamily;
		Result = EvaluateVulkanPhysicalDeviceCandidate(Input);
		EXPECT_EQ(Result.ComputeQueueFamilyIndex, 0);
		EXPECT_EQ(Result.ComputeQueueIndex, 1u);
		Input.QueueFamilies.pop_back();
		Input.ComputeQueuePolicy = EVulkanComputeQueuePolicy::DedicatedFamily;
		Result = EvaluateVulkanPhysicalDeviceCandidate(Input);
		EXPECT_EQ(Result.ComputeQueueFamilyIndex, 0);
		EXPECT_EQ(Result.ComputeQueueIndex, 0u);
		Input.ComputeQueuePolicy = EVulkanComputeQueuePolicy::Automatic;
		Result = EvaluateVulkanPhysicalDeviceCandidate(Input);
		EXPECT_EQ(Result.ComputeQueueIndex, 1u);
		Input.bTimelineSemaphoreFeature = false;
		Result = EvaluateVulkanPhysicalDeviceCandidate(Input);
		EXPECT_FALSE(Result.bEnableTimelineSemaphores);
		EXPECT_EQ(Result.ComputeQueueIndex, 0u);
	}

	TEST(FVulkanDeviceCandidateTests, TimelineFeatureRequiresCoreVersionOrExtensionAndCanStayDisabled)
	{
		auto Input = MakePhysicalDeviceCandidateInput();
		Input.ApiVersion = VK_API_VERSION_1_1;
		Input.bTimelineSemaphoreFeature = true;
		Input.QueueFamilies[0].QueueCount = 2;
		EXPECT_FALSE(EvaluateVulkanPhysicalDeviceCandidate(Input).bEnableTimelineSemaphores);
		Input.AvailableExtensions.emplace_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
		const auto Result = EvaluateVulkanPhysicalDeviceCandidate(Input);
		EXPECT_TRUE(Result.bEnableTimelineSemaphores);
		EXPECT_NE(std::ranges::find(Result.EnabledExtensions, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME), Result.EnabledExtensions.end());
		EXPECT_EQ(Result.ComputeQueueIndex, 0u); // Explicitly disabled policy preserves one queue.
	}

	TEST(FVulkanDeviceCandidateTests, SelectsLowestCompleteQueueAndOptionalFeatures)
	{
		FVulkanPhysicalDeviceCandidateInput Input = MakePhysicalDeviceCandidateInput();
		Input.QueueFamilies.insert(Input.QueueFamilies.begin(),
			{vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute, 1, false});
		Input.bSynchronization2Feature = true;
		Input.bSwapchainMaintenanceFeature = true;
		Input.bHasSwapchainMaintenanceInstanceDependencies = true;
		Input.AvailableExtensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
		const FVulkanPhysicalDeviceCandidateEvaluation Result =
			EvaluateVulkanPhysicalDeviceCandidate(Input);
		ASSERT_TRUE(Result.IsSuitable());
		EXPECT_EQ(Result.GraphicsPresentQueueFamilyIndex, 1);
		EXPECT_TRUE(Result.bEnableSynchronization2);
		EXPECT_TRUE(Result.bEnableSwapchainMaintenance1);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)), 0);
	}

	TEST(FVulkanDeviceCandidateTests,
		HeadlessAdmissionDoesNotClaimPresentationSupport)
	{
		FVulkanPhysicalDeviceCandidateInput Input =
			MakePhysicalDeviceCandidateInput();
		Input.bRequirePresentation = false;
		Input.QueueFamilies[0].bSupportsPresentation = false;

		const FVulkanPhysicalDeviceCandidateEvaluation Result =
			EvaluateVulkanPhysicalDeviceCandidate(Input);
		ASSERT_TRUE(Result.IsSuitable());
		EXPECT_EQ(Result.GraphicsPresentQueueFamilyIndex, 0);
	}

	TEST(FVulkanDeviceCandidateTests,
		RequiresAndEnablesPortabilitySubsetWhenPlatformRequestsIt)
	{
		FVulkanPhysicalDeviceCandidateInput Input =
			MakePhysicalDeviceCandidateInput();
		Input.bRequirePortabilitySubset = true;
		FVulkanPhysicalDeviceCandidateEvaluation Result =
			EvaluateVulkanPhysicalDeviceCandidate(Input);
		ASSERT_FALSE(Result.IsSuitable());
		ASSERT_EQ(Result.RejectionReasons.size(), 1u);
		EXPECT_FALSE(Result.RejectionReasons.front().empty());

		Input.AvailableExtensions.emplace_back(
			"VK_KHR_portability_subset");
		Result = EvaluateVulkanPhysicalDeviceCandidate(Input);
		ASSERT_TRUE(Result.IsSuitable());
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string("VK_KHR_portability_subset")), 1);
	}

	TEST(FVulkanDeviceCandidateTests, RankingIsDeterministic)
	{
		FVulkanPhysicalDeviceCandidateInput Discrete = MakePhysicalDeviceCandidateInput();
		FVulkanPhysicalDeviceCandidateInput Integrated = Discrete;
		Integrated.DeviceType = vk::PhysicalDeviceType::eIntegratedGpu;
		EXPECT_TRUE(IsVulkanPhysicalDeviceCandidatePreferred(Discrete, Integrated));
		Integrated = Discrete;
		Integrated.MaxImageDimension2D = Discrete.MaxImageDimension2D - 1;
		EXPECT_TRUE(IsVulkanPhysicalDeviceCandidatePreferred(Discrete, Integrated));
		Integrated = Discrete;
		Integrated.VendorId = Discrete.VendorId + 1;
		EXPECT_TRUE(IsVulkanPhysicalDeviceCandidatePreferred(Discrete, Integrated));
		Integrated = Discrete;
		Integrated.DeviceName = "Z GPU";
		EXPECT_TRUE(IsVulkanPhysicalDeviceCandidatePreferred(Discrete, Integrated));
	}


	TEST_F(FVulkanCreateFailureInjectionTests, CreationTimingBoundsConcurrentRecordsAndClosesExceptionalNativeCalls)
	{
		BeginVulkanCreationTimingCapture(2);
		std::vector<std::thread> Producers;
		for (uint32 Index = 0; Index < 16; ++Index)
			Producers.emplace_back([] {
				FVulkanCreationTimingScope Request(false);
				auto* Timing = Request.Get();
				Timing->Scheduled = VulkanCreationTimestamp();
				FVulkanCreationBodyTimingScope Body(Timing);
				try
				{
					FVulkanNativeCreationTimingScope Native;
					throw std::runtime_error("timed native failure");
				}
				catch (const std::runtime_error&) {}
			});
		for (auto& Producer : Producers) Producer.join();
		uint64 Dropped = 0;
		const auto Samples = EndVulkanCreationTimingCapture(Dropped);
		ASSERT_EQ(Samples.size(), 2u);
		EXPECT_EQ(Dropped, 14u);
		EXPECT_NE(Samples[0].RequestId, Samples[1].RequestId);
		for (const auto& Sample : Samples)
		{
			EXPECT_FALSE(Sample.bSucceeded);
			EXPECT_GE(Sample.BodyStart, Sample.Scheduled);
			EXPECT_GE(Sample.NativeStart, Sample.BodyStart);
			EXPECT_GE(Sample.NativeEnd, Sample.NativeStart);
			EXPECT_GE(Sample.BodyEnd, Sample.NativeEnd);
			EXPECT_GE(Sample.Returned, Sample.BodyEnd);
		}
		FVulkanCreationTimingScope Disabled(false);
		EXPECT_EQ(Disabled.Get(), nullptr);
	}

	TEST_F(FVulkanCreateFailureInjectionTests, ArmedBoundaryFailsExactlyOnce)
	{
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Image);

		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Image));
		EXPECT_FALSE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Image));
	}

	TEST_F(FVulkanCreateFailureInjectionTests, ArmedBoundariesRemainIndependent)
	{
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Device);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);

		EXPECT_FALSE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain));
		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer));
		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Device));
	}

	TEST_F(FVulkanCreateFailureInjectionTests, NativeBoundaryReportsInjectedOutOfMemory)
	{
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Instance);

		try
		{
			ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Instance);
			FAIL() << "Expected the armed native creation boundary to throw.";
		}
		catch (const vk::SystemError& Error)
		{
			EXPECT_EQ(Error.code().value(), static_cast<int>(vk::Result::eErrorOutOfDeviceMemory));
		}
		EXPECT_NO_THROW(ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Instance));
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		InitializationFailuresRollbackAndReleaseTheBackendModule)
	{
		const std::array FailureCases = {
			EVulkanCreateFailurePoint::Instance,
			EVulkanCreateFailurePoint::Device,
			EVulkanCreateFailurePoint::Allocator,
		};

		for (const auto FailurePoint : FailureCases)
		{
			SCOPED_TRACE(static_cast<int>(FailurePoint));
			ArmVulkanCreateFailure(FailurePoint);

			EXPECT_FALSE(RHIInit(GetVulkanTestInitializationContext()));
			EXPECT_EQ(GDynamicRHI, nullptr);
			EXPECT_FALSE(FModuleManager::Get().IsModuleLoaded("VulkanRHI"));
		}

		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		EXPECT_TRUE(FModuleManager::Get().IsModuleLoaded("VulkanRHI"));
		RHIExit();
		ExpectVulkanModuleUnloaded();
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		OptionalDebugMessengerFailsRecoverablyAndDiesBeforeInstance)
	{
		_putenv_s("DURIN_VULKAN_VALIDATION", "off");
		ResetVulkanDebugMessengerTestStats();
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		auto* VulkanRHI = static_cast<FVulkanDynamicRHI*>(GDynamicRHI);
		EXPECT_FALSE(VulkanRHI->GetDiagnosticAvailability().bRequested);
		EXPECT_FALSE(VulkanRHI->GetDiagnosticAvailability().bDebugUtilsActive);
		EXPECT_FALSE(VulkanRHI->GetDiagnosticAvailability().bMessengerActive);
		RHIExit();
		ExpectVulkanModuleUnloaded();

		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		ResetVulkanDebugMessengerTestStats();
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::DebugMessenger);
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		VulkanRHI = static_cast<FVulkanDynamicRHI*>(GDynamicRHI);
		EXPECT_TRUE(VulkanRHI->GetDiagnosticAvailability().bRequested);
		EXPECT_TRUE(VulkanRHI->GetDiagnosticAvailability().bDebugUtilsSupported);
		EXPECT_TRUE(VulkanRHI->GetDiagnosticAvailability().bDebugUtilsActive);
		EXPECT_FALSE(VulkanRHI->GetDiagnosticAvailability().bMessengerActive);
		EXPECT_EQ(GetVulkanDebugMessengerTestStats().ActiveCount, 0u);
		RHIExit();
		ExpectVulkanModuleUnloaded();

		ResetVulkanDebugMessengerTestStats();
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		VulkanRHI = static_cast<FVulkanDynamicRHI*>(GDynamicRHI);
		EXPECT_TRUE(VulkanRHI->GetDiagnosticAvailability().bMessengerActive);
		FVulkanDebugMessengerTestStats Statistics =
			GetVulkanDebugMessengerTestStats();
		EXPECT_EQ(Statistics.CreatedCount, 1u);
		EXPECT_EQ(Statistics.ActiveCount, 1u);
		RHIExit();
		ExpectVulkanModuleUnloaded();

		Statistics = GetVulkanDebugMessengerTestStats();
		EXPECT_EQ(Statistics.CreatedCount, 1u);
		EXPECT_EQ(Statistics.DestroyedCount, 1u);
		EXPECT_EQ(Statistics.ActiveCount, 0u);
		EXPECT_GT(Statistics.LastMessengerDestroySequence, 0u);
		EXPECT_GT(Statistics.LastInstanceDestroySequence,
			Statistics.LastMessengerDestroySequence);
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		NamesPublicBufferAndReplaysNestedDiagnosticRegions)
	{
		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		ResetVulkanDebugUtilsEventsForTest();

		FRHICommandListImmediate& Immediate = FRHICommandListImmediate::Get();
		FBufferRHIRef Buffer = GDynamicRHI->RHICreateBuffer(
			Immediate, FRHIBufferCreateDesc::Create(
				"Stage2NamedBuffer", 256, 16,
				EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static));
		ASSERT_TRUE(Buffer);
		std::string Utf8BoundaryName(254, 'a');
		Utf8BoundaryName += "\xc3\xa9";
		auto* VulkanRHI = static_cast<FVulkanDynamicRHI*>(GDynamicRHI);
		VulkanRHI->GetDebugUtils().NameObject(
			static_cast<FVulkanBuffer*>(Buffer.GetReference())->GetHandle(),
			Utf8BoundaryName);
		std::string Outer = "Stage2Outer";
		Immediate.BeginDiagnosticRegion(Outer);
		Outer = "MutatedAfterRecording";
		Immediate.BeginDiagnosticRegion("Stage2Inner");
		Immediate.EndDiagnosticRegion();
		Immediate.EndDiagnosticRegion();
		Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::SubmitToGPU);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [] {
			auto* Context = GDynamicRHI->RHIGetDefaultContext();
			Context->RHIBeginDiagnosticRegion("NativeOuter");
			Context->RHIBeginDiagnosticRegion("NativeInner");
			Context->RHISubmitCommands();
			Context->RHIEndDiagnosticRegion();
			Context->RHIEndDiagnosticRegion();
			Context->RHISubmitCommands();
		});

		const std::vector<FVulkanDebugUtilsTestEvent> Events =
			GetVulkanDebugUtilsEventsForTest();
		EXPECT_TRUE(std::ranges::any_of(Events, [](const auto& Event) {
			return Event.Type == EVulkanDebugUtilsTestEventType::ObjectName
				&& Event.ObjectType == vk::ObjectType::eBuffer
				&& Event.Name == "Stage2NamedBuffer";
		}));
		EXPECT_TRUE(std::ranges::any_of(Events, [](const auto& Event) {
			return Event.Type == EVulkanDebugUtilsTestEventType::ObjectName
				&& Event.ObjectType == vk::ObjectType::eBuffer
				&& Event.Name == std::string(254, 'a');
		}));
		std::vector<std::string> LabelEvents;
		for (const auto& Event : Events)
		{
			if (Event.Type == EVulkanDebugUtilsTestEventType::LabelBegin)
				LabelEvents.emplace_back("Begin:" + Event.Name);
			else if (Event.Type == EVulkanDebugUtilsTestEventType::LabelEnd)
				LabelEvents.emplace_back("End");
		}
		EXPECT_EQ(LabelEvents, (std::vector<std::string>{
			"Begin:Stage2Outer", "Begin:Stage2Inner", "End", "End",
			"Begin:NativeOuter", "Begin:NativeInner", "End", "End",
			"Begin:NativeOuter", "Begin:NativeInner", "End", "End"}));

		ResetVulkanDebugUtilsEventsForTest();
		FRHITextureCreateDesc RenderTargetDesc = FRHITextureCreateDesc::Create2D(
			"Stage2RegionRenderTarget", 4, 4, EPixelFormat::RGBA8_UNORM);
		RenderTargetDesc.Flags = ETextureCreateFlags::RenderTargetable;
		FTextureRHIRef RenderTarget =
			GDynamicRHI->RHICreateTexture(Immediate, RenderTargetDesc);
		ASSERT_TRUE(RenderTarget);
		FRHIRenderPassInfo PassInfo;
		PassInfo.RenderTargetLayout.NumColorRenderTargets = 1;
		auto& Attachment =
			PassInfo.RenderTargetLayout.ColorAttachments[0].RenderTarget;
		Attachment.Format = EPixelFormat::RGBA8_UNORM;
		Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
		Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
		Attachment.InitialLayout = ERHITextureLayout::Undefined;
		Attachment.InitialAccess = ERHIAccess::None;
		Attachment.FinalLayout = ERHITextureLayout::ColorAttachment;
		Attachment.FinalAccess = ERHIAccess::ColorAttachmentReadWrite;
		PassInfo.ColorRenderTargets[0] = RenderTarget;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			auto* Context = static_cast<FVulkanCommandListContext*>(
				GDynamicRHI->RHIGetDefaultContext());
			Context->RHIBeginDiagnosticRegion("Stage2OutsidePass");
			Context->RHIBeginRenderPass(PassInfo, "Stage2RenderPass");
			Context->RHIEndRenderPass();
			Context->RHIEndDiagnosticRegion();
		});
		LabelEvents.clear();
		for (const auto& Event : GetVulkanDebugUtilsEventsForTest())
		{
			if (Event.Type == EVulkanDebugUtilsTestEventType::LabelBegin)
				LabelEvents.emplace_back("Begin:" + Event.Name);
			else if (Event.Type == EVulkanDebugUtilsTestEventType::LabelEnd)
				LabelEvents.emplace_back("End");
		}
		EXPECT_EQ(LabelEvents, (std::vector<std::string>{
			"Begin:Stage2OutsidePass", "Begin:Stage2RenderPass", "End", "End"}));

		Buffer = nullptr;
		RenderTarget = nullptr;
		Immediate.ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		PublicTransferTimingBecomesReadyWithoutImplicitWait)
	{
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			FRHICommandListImmediate& Immediate = FRHICommandListImmediate::Get();
			FGPUTimingQueryRHIRef Query =
				GDynamicRHI->RHICreateGPUTimingQuery();
			ASSERT_TRUE(Query);
			EXPECT_EQ(GDynamicRHI->RHIGetGPUTimingResult(Query).State,
				ERHIGPUTimingResultState::Invalid);

			const auto Desc = FRHIBufferCreateDesc::Create(
				"Stage3TimingBuffer", 4096, 4,
				EBufferUsageFlags::SourceCopy | EBufferUsageFlags::DestinationCopy);
			FBufferRHIRef Source = GDynamicRHI->RHICreateBuffer(Immediate, Desc);
			FBufferRHIRef Destination = GDynamicRHI->RHICreateBuffer(Immediate, Desc);
			ASSERT_TRUE(Source && Destination);
			Immediate.BeginGPUTimingQuery(Query);
			Immediate.TransitionBuffers(std::array{
				FRHIBufferTransition{Source, 0, 4096,
					ERHIAccess::Discard, ERHIAccess::TransferRead},
				FRHIBufferTransition{Destination, 0, 4096,
					ERHIAccess::Discard, ERHIAccess::TransferWrite}});
			Immediate.CopyBuffer(Source, Destination,
				std::array{FRHIBufferCopyRegion{0, 0, 4096}});
			Immediate.EndGPUTimingQuery(Query);
			Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
				ERHISubmitFlags::SubmitToGPU);
			EXPECT_EQ(GDynamicRHI->RHIGetGPUTimingResult(Query).State,
				ERHIGPUTimingResultState::Pending);

			FRHIGPUTimingResult Result;
			for (uint32 Attempt = 0; Attempt < 500; ++Attempt)
			{
				GCommandListExecutor.ExecuteSynchronousOperation(false, []() {
					auto* VulkanRHI = static_cast<FVulkanDynamicRHI*>(GDynamicRHI);
					PollVulkanGPUTimingForTest(*VulkanRHI);
				});
				Result = GDynamicRHI->RHIGetGPUTimingResult(Query);
				if (Result.State == ERHIGPUTimingResultState::Ready) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			EXPECT_EQ(Result.State, ERHIGPUTimingResultState::Ready);
			EXPECT_GT(Result.DurationNanoseconds, 0u);
			FRHITextureCreateDesc RenderTargetDesc =
				FRHITextureCreateDesc::Create2D(
					"Stage3TimingRenderTarget", 16, 16, EPixelFormat::RGBA8_UNORM);
			RenderTargetDesc.Flags = ETextureCreateFlags::RenderTargetable;
			FTextureRHIRef RenderTarget =
				GDynamicRHI->RHICreateTexture(Immediate, RenderTargetDesc);
			ASSERT_TRUE(RenderTarget);
			FRHIRenderPassInfo PassInfo;
			PassInfo.RenderTargetLayout.NumColorRenderTargets = 1;
			auto& Attachment =
				PassInfo.RenderTargetLayout.ColorAttachments[0].RenderTarget;
			Attachment.Format = EPixelFormat::RGBA8_UNORM;
			Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
			Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
			Attachment.InitialLayout = ERHITextureLayout::Undefined;
			Attachment.InitialAccess = ERHIAccess::None;
			Attachment.FinalLayout = ERHITextureLayout::ColorAttachment;
			Attachment.FinalAccess = ERHIAccess::ColorAttachmentReadWrite;
			PassInfo.ColorRenderTargets[0] = RenderTarget;
			Immediate.SwitchPipeline(ERHIPipeline::Graphics);
			Immediate.BeginGPUTimingQuery(Query);
			Immediate.BeginRenderPass(PassInfo, "Stage3TimedGraphicsPass");
			Immediate.EndRenderPass();
			Immediate.EndGPUTimingQuery(Query);
			Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
				ERHISubmitFlags::SubmitToGPU);
			EXPECT_EQ(GDynamicRHI->RHIGetGPUTimingResult(Query).State,
				ERHIGPUTimingResultState::Pending);
			for (uint32 Attempt = 0; Attempt < 500; ++Attempt)
			{
				GCommandListExecutor.ExecuteSynchronousOperation(false, []() {
					PollVulkanGPUTimingForTest(
						*static_cast<FVulkanDynamicRHI*>(GDynamicRHI));
				});
				Result = GDynamicRHI->RHIGetGPUTimingResult(Query);
				if (Result.State == ERHIGPUTimingResultState::Ready) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			EXPECT_EQ(Result.State, ERHIGPUTimingResultState::Ready);
			EXPECT_GT(Result.DurationNanoseconds, 0u);

			Query = nullptr;
			Source = nullptr;
			Destination = nullptr;
			RenderTarget = nullptr;
			Immediate.ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);

			FGPUTimingQueryRHIRef Orphaned =
				GDynamicRHI->RHICreateGPUTimingQuery();
			ASSERT_TRUE(Orphaned);
			Immediate.BeginGPUTimingQuery(Orphaned);
			Immediate.EndGPUTimingQuery(Orphaned);
			// Replay without submitting, then drop the caller's last reference.
			// The Vulkan context must own the recorded interval until submission.
			Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			Orphaned = nullptr;
			Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
			Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
				ERHISubmitFlags::SubmitToGPU);
			for (uint32 Attempt = 0; Attempt < 500; ++Attempt)
			{
				GCommandListExecutor.ExecuteSynchronousOperation(false, []() {
					PollVulkanGPUTimingForTest(
						*static_cast<FVulkanDynamicRHI*>(GDynamicRHI));
				});
				if (GetVulkanGPUTimingStatisticsForTest(
					*static_cast<FVulkanDynamicRHI*>(GDynamicRHI))
					.PendingIntervals == 0) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Immediate.ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);
			EXPECT_EQ(GetVulkanGPUTimingStatisticsForTest(
				*static_cast<FVulkanDynamicRHI*>(GDynamicRHI)).LiveIntervals, 0u);
			RHIExit();
			ExpectVulkanModuleUnloaded();
		}
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		TimingQueryPoolFailureExhaustionAndReuseAreRecoverable)
	{
		_putenv_s("DURIN_RHI_EXECUTION", "threaded");
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		auto* VulkanRHI = static_cast<FVulkanDynamicRHI*>(GDynamicRHI);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::QueryPool);
		EXPECT_FALSE(GDynamicRHI->RHICreateGPUTimingQuery());
		auto Statistics = GetVulkanGPUTimingStatisticsForTest(*VulkanRHI);
		EXPECT_EQ(Statistics.AllocatedPages, 0u);
		EXPECT_EQ(Statistics.LiveIntervals, 0u);
		EXPECT_EQ(Statistics.AllocationFailureCount, 1u);

		std::vector<FGPUTimingQueryRHIRef> Queries;
		Queries.reserve(1280);
		for (uint32 Index = 0; Index < 1280; ++Index)
		{
			FGPUTimingQueryRHIRef Query =
				GDynamicRHI->RHICreateGPUTimingQuery();
			ASSERT_TRUE(Query) << Index;
			Queries.push_back(std::move(Query));
		}
		EXPECT_FALSE(GDynamicRHI->RHICreateGPUTimingQuery());
		Statistics = GetVulkanGPUTimingStatisticsForTest(*VulkanRHI);
		EXPECT_EQ(Statistics.IntervalCapacity, 1280u);
		EXPECT_EQ(Statistics.AllocatedPages, 20u);
		EXPECT_EQ(Statistics.LiveIntervals, 1280u);
		EXPECT_EQ(Statistics.IntervalHighWater, 1280u);
		EXPECT_EQ(Statistics.ExhaustionCount, 1u);

		Queries.clear();
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
		Statistics = GetVulkanGPUTimingStatisticsForTest(*VulkanRHI);
		EXPECT_EQ(Statistics.LiveIntervals, 0u);
		FGPUTimingQueryRHIRef Reused =
			GDynamicRHI->RHICreateGPUTimingQuery();
		ASSERT_TRUE(Reused);
		EXPECT_GE(GetVulkanGPUTimingStatisticsForTest(*VulkanRHI).ReuseCount, 1u);
		Reused = nullptr;
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		DiagnosticSnapshotComposesAuthoritiesAndResetPreservesLiveState)
	{
		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		FGPUTimingQueryRHIRef Query = GDynamicRHI->RHICreateGPUTimingQuery();
		ASSERT_TRUE(Query);
		FRHIDiagnosticSnapshot Before;
		FRHIDiagnosticSnapshot Repeated;
		FRHIDiagnosticSnapshot After;
		std::string Formatted;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			Before = GDynamicRHI->RHIGetDiagnosticSnapshot();
			Formatted = FormatRHIDiagnosticSnapshot(Before);
			Repeated = GDynamicRHI->RHIGetDiagnosticSnapshot();
			GDynamicRHI->RHIResetDiagnosticStatistics();
			After = GDynamicRHI->RHIGetDiagnosticSnapshot();
		});

		EXPECT_TRUE(Before.Availability.bRequested);
		EXPECT_TRUE(Before.Availability.bDebugUtilsActive);
		EXPECT_TRUE(Before.Availability.bValidationLayerActive);
		EXPECT_TRUE(Before.Availability.bMessengerActive);
		EXPECT_EQ(Before.Timing.IntervalCapacity, 1280u);
		EXPECT_EQ(Before.Timing.AllocatedPages, 1u);
		EXPECT_EQ(Before.Timing.LiveIntervals, 1u);
		EXPECT_EQ(Before.Executor.Mode, ERHICommandListExecutorMode::Threaded);
		EXPECT_GT(Before.Naming.NamingAttempts, 0u);
		EXPECT_NE(Formatted.find("timing(pages=1,live=1"), std::string::npos);
		EXPECT_EQ(Repeated.Executor.RecordedCommandCount,
			Before.Executor.RecordedCommandCount);
		EXPECT_EQ(Repeated.Executor.SynchronousOperationCount,
			Before.Executor.SynchronousOperationCount);
		EXPECT_EQ(Repeated.Completion.LastSubmittedToken,
			Before.Completion.LastSubmittedToken);
		EXPECT_EQ(Repeated.Messages.Total, Before.Messages.Total);
		EXPECT_EQ(Repeated.Naming.NamingAttempts, Before.Naming.NamingAttempts);
		EXPECT_EQ(Repeated.Timing.ResultPollCount,
			Before.Timing.ResultPollCount);

		EXPECT_EQ(After.Availability.bDebugUtilsActive,
			Before.Availability.bDebugUtilsActive);
		EXPECT_EQ(After.Executor.RecordedCommandCount,
			Before.Executor.RecordedCommandCount);
		EXPECT_EQ(After.PipelineCache.GraphicsPipelines.Capacity,
			Before.PipelineCache.GraphicsPipelines.Capacity);
		EXPECT_EQ(After.PipelineCache.GraphicsPipelines.Occupancy,
			Before.PipelineCache.GraphicsPipelines.Occupancy);
		EXPECT_EQ(After.Completion.LastSubmittedToken,
			Before.Completion.LastSubmittedToken);
		EXPECT_EQ(After.Completion.PendingSubmissions,
			Before.Completion.PendingSubmissions);
		EXPECT_EQ(After.Completion.RetirementPendingCount,
			Before.Completion.RetirementPendingCount);
		EXPECT_EQ(After.Completion.RetirementPendingCount,
			After.Memory.RetirementPendingCount);
		for (uint32 ClassIndex = 0;
			ClassIndex < FRHIMemoryStatistics::AllocationClassCount; ++ClassIndex)
		{
			EXPECT_EQ(After.Memory.Classes[ClassIndex].LiveAllocationCount,
				Before.Memory.Classes[ClassIndex].LiveAllocationCount);
			EXPECT_EQ(After.Memory.Classes[ClassIndex].LiveBytes,
				Before.Memory.Classes[ClassIndex].LiveBytes);
			EXPECT_EQ(After.Memory.Classes[ClassIndex].ArenaLiveBytes,
				Before.Memory.Classes[ClassIndex].ArenaLiveBytes);
		}
		EXPECT_EQ(After.Timing.AllocatedPages, 1u);
		EXPECT_EQ(After.Timing.LiveIntervals, 1u);
		EXPECT_EQ(After.Timing.IntervalHighWater, 1u);
		EXPECT_EQ(After.Timing.ExhaustionCount, 0u);
		EXPECT_EQ(After.Messages.Total, 0u);
		EXPECT_EQ(After.Naming.NamingAttempts, 0u);
		EXPECT_EQ(After.Naming.ActiveRegionDepth, 0u);

		Query = nullptr;
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			After = GDynamicRHI->RHIGetDiagnosticSnapshot();
		});
		EXPECT_EQ(After.Timing.LiveIntervals, 0u);
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		DiagnosticSnapshotPublishesModeIndependentCapabilities)
	{
		std::array<FRHIDiagnosticSnapshot, 2> Snapshots;
		const std::array Modes{"inline", "threaded"};
		_putenv_s("DURIN_VULKAN_VALIDATION", "off");
		for (size_t Index = 0; Index < Modes.size(); ++Index)
		{
			SCOPED_TRACE(Modes[Index]);
			_putenv_s("DURIN_RHI_EXECUTION", Modes[Index]);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			FGPUTimingQueryRHIRef Query = GDynamicRHI->RHICreateGPUTimingQuery();
			ASSERT_TRUE(Query);
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				Snapshots[Index] = GDynamicRHI->RHIGetDiagnosticSnapshot();
			});
			EXPECT_EQ(Snapshots[Index].Executor.Mode, Index == 0
				? ERHICommandListExecutorMode::Inline
				: ERHICommandListExecutorMode::Threaded);
			Query = nullptr;
			FRHICommandListImmediate::Get().ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);
			RHIExit();
			ExpectVulkanModuleUnloaded();
		}

		EXPECT_EQ(Snapshots[0].Availability.bRequested,
			Snapshots[1].Availability.bRequested);
		EXPECT_EQ(Snapshots[0].Availability.bDebugUtilsActive,
			Snapshots[1].Availability.bDebugUtilsActive);
		EXPECT_EQ(Snapshots[0].Timing.IntervalCapacity,
			Snapshots[1].Timing.IntervalCapacity);
		EXPECT_EQ(Snapshots[0].Timing.AllocatedPages,
			Snapshots[1].Timing.AllocatedPages);
		EXPECT_EQ(Snapshots[0].Timing.LiveIntervals,
			Snapshots[1].Timing.LiveIntervals);
		EXPECT_EQ(Snapshots[0].PipelineCache.GraphicsPipelines.Capacity,
			Snapshots[1].PipelineCache.GraphicsPipelines.Capacity);
	}

	TEST_F(FVulkanPublicRHIConformanceTests, DynamicOffsetsReuseSparseArrayDescriptorsAndSelectCorrectPixels)
	{
		const auto ShaderPath = std::filesystem::path(DURIN_TEST_DATA_DIR) / "RecoverableResourceFactories.slang";
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentDynamicMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderError(Compiled.Error);
		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		for (const bool FullValidation : {false, true})
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			SCOPED_TRACE(FullValidation);
			_putenv_s("DURIN_VULKAN_FULL_DESCRIPTOR_VALIDATION", FullValidation ? "on" : "off");
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			auto& Commands = FRHICommandListImmediate::Get();
			{
				auto CreateShader = [&](uint32 Index) {
					const auto& Shader = Compiled.CompiledShaders[Index];
					auto Desc = FRHIShaderCreateDesc::Create(Shader.DebugName.c_str(), Shader.Frequency, *Shader.Code, Shader.Hash);
					Desc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
					return GDynamicRHI->RHICreateShader(Desc);
				};
				auto Vertex = CreateShader(0);
				auto Fragment = CreateShader(1);
				auto Declaration = GDynamicRHI->RHICreateVertexDeclaration({});
				ASSERT_TRUE(Vertex && Fragment && Declaration);
				FRHIRenderTargetLayout Layout;
				Layout.NumColorRenderTargets = 1;
				auto& Attachment = Layout.ColorAttachments[0].RenderTarget;
				Attachment.Format = EPixelFormat::RGBA8_UNORM;
				Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
				Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
				Attachment.InitialLayout = ERHITextureLayout::Undefined;
				Attachment.InitialAccess = ERHIAccess::None;
				Attachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
				Attachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout = Layout;
				Initializer.BoundShaders.VertexShader = Vertex;
				Initializer.BoundShaders.FragmentShader = Fragment;
				Initializer.VertexDeclaration = Declaration;
				Initializer.PipelineLayout.BindingLayouts.resize(3);
				Initializer.PipelineLayout.BindingLayouts[2].BindingLayouts.emplace_back(
					EShaderStageFlags::Fragment, 5, ERHIBindingType::UniformBufferDynamic, 2);
				auto Pipeline = GDynamicRHI->RHICreateGraphicsPipelineState("SparseDynamicOffsets", Initializer);
				ASSERT_TRUE(Pipeline);
				auto Target = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("DynamicOffsetPixels", 8, 8, EPixelFormat::RGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
				ASSERT_TRUE(Target);
				const std::array Colors{std::array<float, 4>{1, 0, 0, 1}, std::array<float, 4>{0, 1, 0, 1}, std::array<float, 4>{0, 0, 1, 1}};
				const uint32 Alignment = static_cast<uint32>(static_cast<FVulkanDynamicRHI*>(GDynamicRHI)
					->GetDeviceForTesting()->GetGpuProperties().limits.minUniformBufferOffsetAlignment);
				const uint32 Stride = std::max(Alignment, 16u);
				auto Buffer = GDynamicRHI->RHICreateBuffer(Commands, FRHIBufferCreateDesc::Create(
					"DynamicOffsetColors", Stride * 3, 16, EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::Static));
				ASSERT_TRUE(Buffer);
				auto* Bytes = static_cast<std::byte*>(GDynamicRHI->RHILockBuffer(Commands, Buffer, 0, Stride * 3, EResourceLockMode::WriteOnly));
				ASSERT_NE(Bytes, nullptr);
				std::array<FRHIUniformBufferRange, 3> Uniforms;
				for (uint32 Index = 0; Index < Colors.size(); ++Index)
				{
					std::memcpy(Bytes + Stride * Index, Colors[Index].data(), sizeof(Colors[Index]));
					Uniforms[Index] = {Buffer.GetReference(), Stride * Index, sizeof(Colors[Index])};
				}
				GDynamicRHI->RHIUnlockBuffer(Commands, Buffer);
				ASSERT_EQ(Uniforms[0].Buffer, Uniforms[2].Buffer);
				ASSERT_NE(Uniforms[0].Offset, Uniforms[2].Offset);
				std::array<FRHIShaderParameterResource, 2> Parameters;
				for (uint32 Index = 0; Index < Parameters.size(); ++Index)
					Parameters[1 - Index] = {.Resource = Uniforms[Index].Buffer, .SetIndex = 2, .BindingIndex = 5,
						.ArrayElement = Index, .Type = ERHIBindingType::UniformBufferDynamic,
						.Offset = Uniforms[Index].Offset, .Size = Uniforms[Index].Size};
				auto First = FRHIShaderParameterBatch::Create(Fragment, Parameters);
				Parameters[1].Offset = Uniforms[2].Offset;
				auto Second = FRHIShaderParameterBatch::Create(Fragment, Parameters);
				ASSERT_TRUE(First && Second);
				ASSERT_EQ(First->GetParameters()[1].Resource, Second->GetParameters()[1].Resource);
				ResetVulkanHotPathWorkTestStats();
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				FRHIRenderPassInfo Pass;
				Pass.RenderTargetLayout = Layout;
				Pass.ColorRenderTargets[0] = Target;
				Commands.BeginRenderPass(Pass, "DynamicOffsetArrays");
				Commands.SetGraphicsPipelineState(*Pipeline);
				Commands.SetViewport(0, 0, 0, 4, 8, 1);
				Commands.SetPreparedShaderParameters(First);
				Commands.Draw({.VertexCount = 3});
				Commands.SetViewport(4, 0, 0, 8, 8, 1);
				Commands.SetPreparedShaderParameters(Second);
				Commands.Draw({.VertexCount = 3});
				Commands.SetPreparedShaderParameters(Second);
				Commands.Draw({.VertexCount = 3});
				Commands.EndRenderPass();
				FByteBuffer Pixels;
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Target, 0, 0, Pixels));
				ASSERT_EQ(Pixels.size(), 256u);
				const size_t Left = (4 * 8 + 1) * 4;
				const size_t Right = (4 * 8 + 6) * 4;
				EXPECT_NEAR(std::to_integer<uint8>(Pixels[Left]), 128, 1);
				EXPECT_NEAR(std::to_integer<uint8>(Pixels[Left + 1]), 128, 1);
				EXPECT_EQ(Pixels[Left + 2], std::byte{0});
				EXPECT_EQ(Pixels[Right], std::byte{0});
				EXPECT_NEAR(std::to_integer<uint8>(Pixels[Right + 1]), 128, 1);
				EXPECT_NEAR(std::to_integer<uint8>(Pixels[Right + 2]), 128, 1);
				const auto Work = GetVulkanHotPathWorkTestStats();
				EXPECT_EQ(Work.DescriptorSorts, 1u);
				EXPECT_EQ(Work.DescriptorHashes, 3u);
				EXPECT_EQ(Work.DescriptorOwnerRebuilds, 1u);
				EXPECT_EQ(Work.BindingValidationVisits, FullValidation ? 6u : 2u);
				EXPECT_EQ(Work.DescriptorDrawValidationVisits, 6u);
			}
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
			RHIExit();
			ExpectVulkanModuleUnloaded();
		}
	}

	TEST_F(FVulkanPublicRHIConformanceTests, DescriptorSetsReuseCommonStateAcrossMaterialAndCompatiblePipelineChanges)
	{
		const auto ShaderPath = std::filesystem::path(DURIN_TEST_DATA_DIR) / "RecoverableResourceFactories.slang";
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentFrequencyMain", "FragmentFrequencyAlternateMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Fragment};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderError(Compiled.Error);
		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			auto& Commands = FRHICommandListImmediate::Get();
			{
				auto CreateShader = [&](uint32 Index) {
					const auto& Shader = Compiled.CompiledShaders[Index];
					auto Desc = FRHIShaderCreateDesc::Create(Shader.DebugName.c_str(), Shader.Frequency, *Shader.Code, Shader.Hash);
					Desc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
					return GDynamicRHI->RHICreateShader(Desc);
				};
				auto Vertex = CreateShader(0);
				auto Fragment = CreateShader(1);
				auto AlternateFragment = CreateShader(2);
				auto Declaration = GDynamicRHI->RHICreateVertexDeclaration({});
				ASSERT_TRUE(Vertex && Fragment && AlternateFragment && Declaration);
				FRHIRenderTargetLayout Layout;
				Layout.NumColorRenderTargets = 1;
				auto& Attachment = Layout.ColorAttachments[0].RenderTarget;
				Attachment.Format = EPixelFormat::RGBA8_UNORM;
				Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
				Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
				Attachment.InitialLayout = ERHITextureLayout::Undefined;
				Attachment.InitialAccess = ERHIAccess::None;
				Attachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
				Attachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout = Layout;
				Initializer.BoundShaders.VertexShader = Vertex;
				Initializer.BoundShaders.FragmentShader = Fragment;
				Initializer.VertexDeclaration = Declaration;
				Initializer.PipelineLayout.BindingLayouts.resize(3);
				Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
					EShaderStageFlags::Fragment, 3, ERHIBindingType::UniformBuffer);
				Initializer.PipelineLayout.BindingLayouts[2].BindingLayouts.emplace_back(
					EShaderStageFlags::Fragment, 7, ERHIBindingType::UniformBuffer);
				auto Pipeline = GDynamicRHI->RHICreateGraphicsPipelineState("FrequencySets", Initializer);
				Initializer.BoundShaders.FragmentShader = AlternateFragment;
				auto Compatible = GDynamicRHI->RHICreateGraphicsPipelineState("CompatibleFrequencySets", Initializer);
				Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts.clear();
				Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
					EShaderStageFlags::Vertex | EShaderStageFlags::Fragment, 3, ERHIBindingType::UniformBuffer);
				auto Incompatible = GDynamicRHI->RHICreateGraphicsPipelineState("IncompatibleFrequencySet", Initializer);
				ASSERT_TRUE(Pipeline && Compatible && Incompatible);
				ASSERT_NE(Pipeline.GetReference(), Compatible.GetReference());
				ASSERT_NE(Compatible.GetReference(), Incompatible.GetReference());
				auto Target = GDynamicRHI->RHICreateTexture(Commands,
					FRHITextureCreateDesc::Create2D("DynamicOffsetPixels", 8, 8, EPixelFormat::RGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
				ASSERT_TRUE(Target);
				const std::array Colors{std::array<float, 4>{1, 1, 1, 1}, std::array<float, 4>{1, 0, 0, 1}, std::array<float, 4>{0, 1, 0, 1}};
				std::array<FBufferRHIRef, 3> Buffers;
				for (uint32 Index = 0; Index < Buffers.size(); ++Index)
				{
					Buffers[Index] = GDynamicRHI->RHICreateBuffer(Commands, FRHIBufferCreateDesc::Create(
						"FrequencyColor", 16, 16, EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::Static));
					ASSERT_TRUE(Buffers[Index]);
					auto* Bytes = GDynamicRHI->RHILockBuffer(Commands, Buffers[Index], 0, 16, EResourceLockMode::WriteOnly);
					ASSERT_NE(Bytes, nullptr);
					std::memcpy(Bytes, Colors[Index].data(), 16);
					GDynamicRHI->RHIUnlockBuffer(Commands, Buffers[Index]);
				}
				auto MakeBatch = [&](FRHIShader* Shader, uint32 BufferIndex, uint32 Set, uint32 Binding) {
					const FRHIShaderParameterResource Resource{.Resource = Buffers[BufferIndex].GetReference(),
						.SetIndex = Set, .BindingIndex = Binding, .Type = ERHIBindingType::UniformBuffer, .Size = 16};
					return FRHIShaderParameterBatch::Create(Shader, std::span<const FRHIShaderParameterResource>(&Resource, 1));
				};
				auto Common = MakeBatch(Fragment, 0, 0, 3);
				auto Red = MakeBatch(Fragment, 1, 2, 7);
				auto Green = MakeBatch(Fragment, 2, 2, 7);
				auto AlternateCommon = MakeBatch(AlternateFragment, 0, 0, 3);
				auto AlternateRed = MakeBatch(AlternateFragment, 1, 2, 7);
				ASSERT_TRUE(Common && Red && Green && AlternateCommon && AlternateRed);
				ResetVulkanHotPathWorkTestStats();
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				FRHIRenderPassInfo Pass;
				Pass.RenderTargetLayout = Layout;
				Pass.ColorRenderTargets[0] = Target;
				Commands.BeginRenderPass(Pass, "DynamicOffsetArrays");
				Commands.SetGraphicsPipelineState(*Pipeline);
				Commands.SetViewport(0, 0, 0, 2, 8, 1);
				Commands.SetPreparedShaderParameters(Common);
				Commands.SetPreparedShaderParameters(Red);
				Commands.Draw({.VertexCount = 3});
				Commands.SetViewport(2, 0, 0, 4, 8, 1);
				Commands.SetPreparedShaderParameters(Green);
				Commands.Draw({.VertexCount = 3});
				Commands.SetGraphicsPipelineState(*Compatible);
				Commands.SetViewport(4, 0, 0, 6, 8, 1);
				Commands.SetPreparedShaderParameters(AlternateCommon);
				Commands.SetPreparedShaderParameters(AlternateRed);
				Commands.Draw({.VertexCount = 3});
				Commands.SetGraphicsPipelineState(*Incompatible);
				Commands.SetViewport(6, 0, 0, 8, 8, 1);
				Commands.SetPreparedShaderParameters(AlternateCommon);
				Commands.SetPreparedShaderParameters(AlternateRed);
				Commands.Draw({.VertexCount = 3});
				Commands.EndRenderPass();
				FByteBuffer Pixels;
				ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Target, 0, 0, Pixels));
				ASSERT_EQ(Pixels.size(), 256u);
				for (uint32 Column = 0; Column < 4; ++Column)
				{
					const size_t Pixel = (4 * 8 + Column * 2) * 4;
					EXPECT_EQ(Pixels[Pixel], Column == 1 ? std::byte{0} : std::byte{255});
					EXPECT_EQ(Pixels[Pixel + 1], Column == 1 ? std::byte{255} : std::byte{0});
					EXPECT_EQ(Pixels[Pixel + 2], std::byte{0});
				}
				const auto Work = GetVulkanHotPathWorkTestStats();
				EXPECT_EQ(Work.DescriptorHashes, 10u);
				EXPECT_EQ(Work.DescriptorOwnerRebuilds, 7u);
				FRHIDiagnosticSnapshot Snapshot;
				GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
					Snapshot = GDynamicRHI->RHIGetDiagnosticSnapshot();
				});
				// Common, empty, red, green, and the incompatible common layout.
				EXPECT_EQ(Snapshot.PipelineCache.DescriptorSnapshots.NativeCreations, 5u);
				EXPECT_EQ(Snapshot.PipelineCache.DescriptorSnapshots.Occupancy, 5u);
				EXPECT_EQ(Snapshot.PipelineCache.DescriptorValueOccupancy, 4u);
				EXPECT_EQ(Snapshot.PipelineCache.DescriptorSnapshots.Hits, 7u);

				// Freeze three complete command chunks before any pool retirement.
				// Each must initialize all sparse sets and remain replayable after the
				// preceding chunk's descriptor allocation lease has retired.
				std::array<FRHICommandList, 3> Chunks;
				for (uint32 Index = 0; Index < Chunks.size(); ++Index)
				{
					auto& Chunk = Chunks[Index];
					Chunk.SwitchPipeline(ERHIPipeline::Graphics);
					Chunk.BeginRenderPass(Pass, "FrozenDescriptorChunk");
					Chunk.SetGraphicsPipelineState(*Pipeline);
					Chunk.SetViewport(0, 0, 0, 8, 8, 1);
					Chunk.SetPreparedShaderParameters(Common);
					Chunk.SetPreparedShaderParameters(Index == 1 ? Green : Red);
					Chunk.Draw({.VertexCount = 3});
					Chunk.EndRenderPass();
					Chunk.FinishRecording();
				}
				for (uint32 Index = 0; Index < Chunks.size(); ++Index)
				{
					if (Index != 0)
						GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
							SubmitAndRetireDescriptorPoolsForTesting();
							WaitForAllVulkanSubmissionsForTesting();
						});
					Commands.QueueCommandList(std::move(Chunks[Index]));
					ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Target, 0, 0, Pixels));
					ASSERT_EQ(Pixels.size(), 256u);
					const size_t Center = (4 * 8 + 4) * 4;
					EXPECT_EQ(Pixels[Center], Index == 1 ? std::byte{0} : std::byte{255});
					EXPECT_EQ(Pixels[Center + 1], Index == 1 ? std::byte{255} : std::byte{0});
					GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
						Snapshot = GDynamicRHI->RHIGetDiagnosticSnapshot();
					});
					EXPECT_EQ(Snapshot.PipelineCache.DescriptorSnapshots.NativeCreations, 5u + Index * 3u);
				}

			}
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
			RHIExit();
			ExpectVulkanModuleUnloaded();
		}
	}

	TEST_F(FVulkanPublicRHIConformanceTests,
		PublicRHIConformanceDrawMatchesPixelsAndDiagnosticsAcrossModes)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "RecoverableResourceFactories.slang";
		FShaderCompileOptions CompileOptions;
		CompileOptions.EntryPoints = {"VertexMain", "FragmentMain"};
		CompileOptions.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput CompileOutput =
			Compiler.Compile(ShaderPath.string(), CompileOptions);
		ASSERT_TRUE(CompileOutput) << FormatShaderError(CompileOutput.Error);
		ASSERT_EQ(CompileOutput.CompiledShaders.size(), 2u);

		std::array<Durin::FByteBuffer, 2> ModePixels;
		std::array<FRHIDiagnosticSnapshot, 2> ModeSnapshots;
		const std::array Modes{"inline", "threaded"};
		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		for (size_t ModeIndex = 0; ModeIndex < Modes.size(); ++ModeIndex)
		{
			SCOPED_TRACE(Modes[ModeIndex]);
			_putenv_s("DURIN_RHI_EXECUTION", Modes[ModeIndex]);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			ResetVulkanHotPathWorkTestStats();
			FRHICommandListImmediate& Commands = FRHICommandListImmediate::Get();

			auto MakeShaderDesc = [](const FCompiledShader& Shader) {
				FRHIShaderCreateDesc Desc = FRHIShaderCreateDesc::Create(
					Shader.DebugName.c_str(), Shader.Frequency, *Shader.Code,
					Shader.Hash);
				Desc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
				return Desc;
			};
			FShaderRHIRef VertexShader = GDynamicRHI->RHICreateShader(
				MakeShaderDesc(CompileOutput.CompiledShaders[0]));
			FShaderRHIRef FragmentShader = GDynamicRHI->RHICreateShader(
				MakeShaderDesc(CompileOutput.CompiledShaders[1]));
			FVertexDeclarationRHIRef VertexDeclaration =
				GDynamicRHI->RHICreateVertexDeclaration({});
			ASSERT_TRUE(VertexShader && FragmentShader && VertexDeclaration);

			FRHITextureCreateDesc SampledDesc = FRHITextureCreateDesc::Create2D(
				"ConformanceSampledTexture", 4, 4, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Sampled =
				GDynamicRHI->RHICreateTexture(Commands, SampledDesc);
			ASSERT_TRUE(Sampled);
			const std::array<uint8, 64> SampledBytes = [] {
				std::array<uint8, 64> Bytes{};
				Bytes.fill(0x7f);
				return Bytes;
			}();
			GDynamicRHI->RHIUpdateTexture2D(Commands, Sampled, 0, 0,
				FUpdateTextureRegion2D(0, 0, 0, 0, 4, 4), 16,
				std::as_bytes(std::span{SampledBytes}));
			FTextureViewRHIRef SampledView = GDynamicRHI->RHICreateTextureView(
				Sampled, MakeDefaultTextureViewDesc(
					*Sampled, ERHITextureViewUsage::Sampled));
			TRefCountPtr<FRHISampler> Sampler =
				GDynamicRHI->RHICreateSampler(FRHISamplerDesc{});
			ASSERT_TRUE(SampledView && Sampler);

			FRHIRenderTargetLayout Layout;
			Layout.NumColorRenderTargets = 1;
			auto& Attachment = Layout.ColorAttachments[0].RenderTarget;
			Attachment.Format = EPixelFormat::RGBA8_UNORM;
			Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
			Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
			Attachment.InitialLayout = ERHITextureLayout::Undefined;
			Attachment.InitialAccess = ERHIAccess::None;
			Attachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
			Attachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = Layout;
			Initializer.BoundShaders.VertexShader = VertexShader;
			Initializer.BoundShaders.FragmentShader = FragmentShader;
			Initializer.VertexDeclaration = VertexDeclaration;
			auto& Bindings = Initializer.PipelineLayout.BindingLayouts
				.emplace_back().BindingLayouts;
			Bindings.emplace_back(
				EShaderStageFlags::Vertex, 0, ERHIBindingType::Texture);
			Bindings.emplace_back(
				EShaderStageFlags::Fragment, 1, ERHIBindingType::Sampler, 2);
			FGraphicsPipelineStateRHIRef Pipeline =
				GDynamicRHI->RHICreateGraphicsPipelineState(
					"PublicRHIConformancePipeline", Initializer);
			ASSERT_TRUE(Pipeline);

			const FRHICapabilities* Capabilities = GDynamicRHI->RHIGetCapabilities();
			ASSERT_NE(Capabilities, nullptr);
			FRHITextureCreateDesc TargetDesc = FRHITextureCreateDesc::Create2D(
				"PublicRHIConformanceTarget", 8, 8, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::CPUReadback);
			FTextureRHIRef Target =
				GDynamicRHI->RHICreateTexture(Commands, TargetDesc);
			ASSERT_TRUE(Target);
			FGPUTimingQueryRHIRef Timing =
				GDynamicRHI->RHICreateGPUTimingQuery();
			ASSERT_TRUE(Timing);

			FRHIRenderPassInfo Pass;
			Pass.RenderTargetLayout = Layout;
			Pass.ColorRenderTargets[0] = Target;
			Pass.ColorClearValues[0] = FClearValueBinding(0, 0, 0, 1);
			Commands.BeginDiagnosticRegion("PublicRHI.Conformance.Draw");
			Commands.BeginGPUTimingQuery(Timing);
			Commands.SwitchPipeline(ERHIPipeline::Graphics);
			Commands.BeginRenderPass(Pass, "PublicRHIConformancePass");
			Commands.SetGraphicsPipelineState(*Pipeline);
			Commands.SetViewport(0, 0, 0, 8, 8, 1);
			std::array<FRHIShaderParameterResource, 1> TextureParameters{
				FRHIShaderParameterResource{.Resource = SampledView.GetReference(),
					.SetIndex = 0, .BindingIndex = 0, .ArrayElement = 0,
					.Type = ERHIBindingType::Texture}};
			std::array<FRHIShaderParameterResource, 2> SamplerParameters{
				FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
					.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 0,
					.Type = ERHIBindingType::Sampler},
				FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
					.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 1,
					.Type = ERHIBindingType::Sampler}};
			Commands.SetShaderParameters(VertexShader, TextureParameters);
			Commands.SetShaderParameters(FragmentShader, SamplerParameters);
			Commands.Draw({.VertexCount = 3});
			Commands.SetShaderParameters(VertexShader, TextureParameters);
			Commands.SetShaderParameters(FragmentShader, SamplerParameters);
			Commands.Draw({.VertexCount = 3});
			for (uint32 SnapshotIndex = 1; SnapshotIndex <= 512; ++SnapshotIndex)
			{
				SamplerParameters[0].Offset = SnapshotIndex;
				Commands.SetShaderParameters(FragmentShader, SamplerParameters);
				Commands.Draw({.VertexCount = 3});
			}
			Commands.EndRenderPass();
			Commands.EndGPUTimingQuery(Timing);
			Commands.EndDiagnosticRegion();
			ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
				Commands, Target, 0, 0, ModePixels[ModeIndex]));
			ASSERT_EQ(ModePixels[ModeIndex].size(), 8u * 8u * 4u);
			EXPECT_NEAR(std::to_integer<uint8>(ModePixels[ModeIndex][0]), 64, 1);
			EXPECT_NEAR(std::to_integer<uint8>(ModePixels[ModeIndex][1]), 128, 1);
			EXPECT_NEAR(std::to_integer<uint8>(ModePixels[ModeIndex][2]), 191, 1);
			EXPECT_EQ(ModePixels[ModeIndex][3], std::byte{255});

			Target = nullptr;
			TargetDesc.DebugName = "PublicRHIConformanceReplacement";
			FTextureRHIRef Replacement =
				GDynamicRHI->RHICreateTexture(Commands, TargetDesc);
			ASSERT_TRUE(Replacement);
			Commands.ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				PollVulkanGPUTimingForTest(
					*static_cast<FVulkanDynamicRHI*>(GDynamicRHI));
				ModeSnapshots[ModeIndex] =
					GDynamicRHI->RHIGetDiagnosticSnapshot();
			});
			EXPECT_EQ(GDynamicRHI->RHIGetGPUTimingResult(Timing).State,
				ERHIGPUTimingResultState::Ready);
			EXPECT_GT(ModeSnapshots[ModeIndex].Naming.LabelBegins, 0u);
			EXPECT_GT(ModeSnapshots[ModeIndex].Timing.ReadyResultCount, 0u);
			EXPECT_EQ(ModeSnapshots[ModeIndex].Naming.ActiveRegionDepth, 0u);
			EXPECT_EQ(ModeSnapshots[ModeIndex].PipelineCache
				.DescriptorSnapshots.Occupancy, 512u);
			EXPECT_EQ(ModeSnapshots[ModeIndex].PipelineCache
				.DescriptorValueOccupancy, 1536u);
			EXPECT_GE(ModeSnapshots[ModeIndex].PipelineCache
				.DescriptorSnapshots.Hits, 1u);
			EXPECT_GE(ModeSnapshots[ModeIndex].PipelineCache
				.DescriptorSnapshots.Evictions, 1u);
			const FVulkanHotPathWorkTestStats HotPathWork =
				GetVulkanHotPathWorkTestStats();
			EXPECT_EQ(HotPathWork.BindingValidationVisits, 1539u);
			EXPECT_EQ(HotPathWork.DescriptorDrawValidationVisits, 514u);
			EXPECT_EQ(HotPathWork.DescriptorSorts, 1u);
			EXPECT_EQ(HotPathWork.DescriptorHashes, 513u);
			EXPECT_EQ(HotPathWork.DescriptorOwnerRebuilds, 2u);
			EXPECT_EQ(HotPathWork.DescriptorOccupancyVerificationVisits, 0u);
			EXPECT_EQ(HotPathWork.DescriptorOccupancyMutations, 514u);

			FRHIDiagnosticSnapshot StatisticsReset;
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				GDynamicRHI->RHIResetDiagnosticStatistics();
				StatisticsReset = GDynamicRHI->RHIGetDiagnosticSnapshot();
			});
			EXPECT_EQ(StatisticsReset.PipelineCache.DescriptorSnapshots.Occupancy, 512u);
			EXPECT_EQ(StatisticsReset.PipelineCache.DescriptorValueOccupancy, 1536u);
			EXPECT_EQ(StatisticsReset.PipelineCache.DescriptorSnapshots.Hits, 0u);

			Replacement = nullptr;
			Timing = nullptr;
			Pipeline = nullptr;
			Sampler = nullptr;
			SampledView = nullptr;
			Sampled = nullptr;
			FragmentShader = nullptr;
			VertexShader = nullptr;
			VertexDeclaration = nullptr;
			Commands.ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);
			RHIExit();
			ExpectVulkanModuleUnloaded();
		}

		EXPECT_EQ(ModePixels[0], ModePixels[1]);
		EXPECT_EQ(ModeSnapshots[0].Timing.IntervalCapacity,
			ModeSnapshots[1].Timing.IntervalCapacity);
		EXPECT_EQ(ModeSnapshots[0].Naming.ActiveRegionDepth,
			ModeSnapshots[1].Naming.ActiveRegionDepth);
	}

	TEST_F(FVulkanCreateFailureInjectionTests, NativeResourceFactoriesRunOutsideReplayAndRollbackPublication)
	{
		FShaderCompileOptions Options;
		Options.EntryPoints = {"ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR) / "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderError(Compiled.Error);
		for (const char* Mode : {"threaded", "inline"})
		{
			SCOPED_TRACE(Mode);
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			auto& Commands = FRHICommandListImmediate::Get();
			const auto BufferDesc = FRHIBufferCreateDesc::Create("IndependentBuffer", 256, 0, EBufferUsageFlags::FormattedBuffer);
			const auto TextureDesc = FRHITextureCreateDesc::Create2D("IndependentTexture", 4, 4, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			auto Buffer = GDynamicRHI->RHICreateBuffer(Commands, BufferDesc);
			auto Texture = GDynamicRHI->RHICreateTexture(Commands, TextureDesc);
			ASSERT_TRUE(Buffer); ASSERT_TRUE(Texture);
			const auto BufferViewDesc = MakeDefaultBufferViewDesc(*Buffer, ERHIBufferViewType::Formatted, EPixelFormat::R32_UINT);
			const auto TextureViewDesc = MakeDefaultTextureViewDesc(*Texture, ERHITextureViewUsage::Sampled);
			const auto& Shader = Compiled.CompiledShaders[0];
			auto ShaderDesc = FRHIShaderCreateDesc::Create("IndependentShader", Shader.Frequency, *Shader.Code, Shader.Hash);
			ShaderDesc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
			const std::array<std::function<TRefCountPtr<FRHIResource>()>, 6> Factories{
				[&] { return GDynamicRHI->RHICreateShader(ShaderDesc); },
				[&] { return GDynamicRHI->RHICreateSampler({}); },
				[&] { return GDynamicRHI->RHICreateBuffer(Commands, BufferDesc); },
				[&] { return GDynamicRHI->RHICreateTexture(Commands, TextureDesc); },
				[&] { return GDynamicRHI->RHICreateBufferView(Buffer, BufferViewDesc); },
				[&] { return GDynamicRHI->RHICreateTextureView(Texture, TextureViewDesc); }};
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
			const auto BeforeMemory = GDynamicRHI->RHIGetMemoryStatistics();
			std::promise<void> Entered, Release;
			auto EnteredFuture = Entered.get_future();
			auto Released = Release.get_future().share();
			if (GRHIThread)
			{
				Commands.EnqueueLambda([&] { Entered.set_value(); Released.wait(); }, 0);
				GCommandListExecutor.Submit({}, ERHISubmitFlags::None);
				EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
			}
			const auto Before = GCommandListExecutor.GetStats();
			std::vector<std::future<bool>> Workers;
			for (uint32 Index = 0; Index < 16; ++Index)
				Workers.push_back(std::async(std::launch::async, [&] {
					bool Complete = true;
					for (const auto& Factory : Factories) { auto Resource = Factory(); Complete = !!Resource && Complete; }
					return Complete;
				}));
			for (auto& Worker : Workers) EXPECT_EQ(Worker.wait_for(std::chrono::seconds(5)), std::future_status::ready);
			Release.set_value();
			for (auto& Worker : Workers) EXPECT_TRUE(Worker.get());
			const auto After = GCommandListExecutor.GetStats();
			EXPECT_EQ(After.SynchronousOperationCount, Before.SynchronousOperationCount);
			EXPECT_EQ(After.LastSubmittedSerial, Before.LastSubmittedSerial);
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
			GDynamicRHI->RHIBlockUntilGPUIdle();
			GCommandListExecutor.ExecuteSynchronousOperation(false, [] {
				ReleaseCompletedVulkanResourcesForTesting();
			});
			for (const auto& Factory : Factories)
			{
				ArmVulkanCreateFailure(EVulkanCreateFailurePoint::ResourcePublication);
				EXPECT_FALSE(Factory());
				EXPECT_FALSE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::ResourcePublication));
			}
			const auto AfterMemory = GDynamicRHI->RHIGetMemoryStatistics();
			for (size_t Index = 0; Index < BeforeMemory.Classes.size(); ++Index)
			{
				EXPECT_EQ(AfterMemory.Classes[Index].LiveAllocationCount, BeforeMemory.Classes[Index].LiveAllocationCount);
				EXPECT_EQ(AfterMemory.Classes[Index].LiveBytes, BeforeMemory.Classes[Index].LiveBytes);
			}
			Buffer = nullptr; Texture = nullptr;
			RHIExit();
		}
	}

	TEST_F(FVulkanCreateFailureInjectionTests, VertexDeclarationsDoNotDependOnReplay)
	{
		for (const char* Mode : {"inline", "threaded"})
		{
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			std::promise<void> ReleaseReplay;
			auto Released = ReleaseReplay.get_future().share();
			FRHICommandListFence HeldFence;
			if (GRHIThread)
			{
				auto Started = std::make_shared<std::promise<void>>();
				auto StartedFuture = Started->get_future();
				GCommandListExecutor.GetImmediateCommandList().EnqueueLambda([Started, Released] {
					Started->set_value();
					Released.wait();
				}, 0);
				GCommandListExecutor.Submit({}, ERHISubmitFlags::None);
				HeldFence = GCommandListExecutor.CreateFence();
				EXPECT_EQ(StartedFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
			}
			const auto Before = GCommandListExecutor.GetStats();
			const auto BeforeSerial = GCommandListExecutor.GetLastSubmittedSerial();
			FVertexDeclarationElementList Elements{};
			Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, 12);
			auto Pending = std::async(std::launch::async, [&] {
				return GDynamicRHI->RHICreateVertexDeclaration(Elements);
			});
			// A regression may enqueue; release replay before get() so failure cannot deadlock teardown.
			EXPECT_EQ(Pending.wait_for(std::chrono::seconds(5)), std::future_status::ready);
			ReleaseReplay.set_value();
			auto Declaration = Pending.get();
			if (GRHIThread) HeldFence.Wait();
			ASSERT_TRUE(Declaration);
			Elements[0].Offset = 4;
			EXPECT_EQ(Declaration->GetElements()[0].Offset, 0);
			EXPECT_EQ(Declaration->GetElements()[0].Stride, 12);
			ArmVulkanCreateFailure(EVulkanCreateFailurePoint::VertexDeclaration);
			EXPECT_FALSE(GDynamicRHI->RHICreateVertexDeclaration(Elements));
			auto Recovered = GDynamicRHI->RHICreateVertexDeclaration(Elements);
			ASSERT_TRUE(Recovered);
			EXPECT_EQ(Recovered->GetElements()[0].Offset, 4);
			const auto After = GCommandListExecutor.GetStats();
			EXPECT_EQ(After.SynchronousOperationCount, Before.SynchronousOperationCount);
			EXPECT_EQ(GCommandListExecutor.GetLastSubmittedSerial(), BeforeSerial);
			Recovered = nullptr;
			Declaration = nullptr;
			RHIExit();
		}
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		CreationBoundaryPreservesTerminalErrorTypes)
	{
		// Exception translation has no device or replay-thread dependency.
		const auto Recoverable = ExecuteFallibleRHICreationOperation(MakeVulkanCreationOperation([] {
			throw vk::OutOfDeviceMemoryError("expected allocation failure");
		}));
		EXPECT_TRUE(Recoverable.HasError());
		EXPECT_EQ(Recoverable.Failure, ERHIResourceCreationFailure::OutOfMemory);
		EXPECT_EQ(Recoverable.NativeCode, static_cast<int32>(vk::Result::eErrorOutOfDeviceMemory));
		EXPECT_EQ(Recoverable.Source, ERHICreationFailureSource::NativeBackend);
		const auto Unsupported = ExecuteFallibleRHICreationOperation(MakeVulkanCreationOperation([] {
			throw vk::FormatNotSupportedError("unsupported descriptor");
		}));
		EXPECT_EQ(Unsupported.Failure,
			ERHIResourceCreationFailure::UnsupportedDescriptor);
		EXPECT_THROW(ExecuteFallibleRHICreationOperation(MakeVulkanCreationOperation([] {
			throw vk::DeviceLostError("terminal device loss");
		})), vk::DeviceLostError);
		EXPECT_THROW(ExecuteFallibleRHICreationOperation(MakeVulkanCreationOperation([] {
			throw std::logic_error("internal invariant failure");
		})), std::logic_error);
		EXPECT_THROW(ExecuteFallibleRHICreationOperation(MakeVulkanCreationOperation([] {
			throw 7;
		})), int);
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		InlineRuntimeFactoryFailureReturnsNullThenRecovers)
	{
		_putenv_s("DURIN_RHI_EXECUTION", "inline");
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		EXPECT_EQ(GRHIThread, nullptr);
		ResetVulkanMemoryBaselineStatistics();

		FRHICommandListImmediate& RHICmdList =
			FRHICommandListImmediate::Get();
		const FRHIBufferCreateDesc BufferDesc = FRHIBufferCreateDesc::Create(
			"RecoverableInlineBuffer", 256, 16,
			EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);
		EXPECT_FALSE(GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc));
		EXPECT_EQ(GDynamicRHI->RHIGetMemoryStatistics().Classes[
			static_cast<uint32>(ERHIMemoryAllocationClass::DeviceLocal)]
			.AllocationFailureCount, 1u);
		FBufferRHIRef Buffer =
			GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc);
		ASSERT_TRUE(Buffer);

		Buffer = nullptr;
		RHICmdList.ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		CapabilitiesAndExactTextureSupportRejectBeforeNativeCreation)
	{
		_putenv_s("DURIN_RHI_EXECUTION", "inline");
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		const FRHICapabilities* Capabilities = GDynamicRHI->RHIGetCapabilities();
		ASSERT_NE(Capabilities, nullptr);
		EXPECT_EQ(Capabilities->FeatureLevel, ERHIFeatureLevel::ES3_1);
		EXPECT_EQ(Capabilities->SupportedTextureDimensions,
			ERHITextureDimensionFlags::Texture2D
				| ERHITextureDimensionFlags::Texture2DArray
				| ERHITextureDimensionFlags::Texture3D
				| ERHITextureDimensionFlags::TextureCube);
		EXPECT_GE(Capabilities->MaxTextureDimension2D, 1u);
		EXPECT_GE(Capabilities->MaxTextureDimension3D, 1u);
		EXPECT_GE(Capabilities->MaxTextureDimensionCube, 1u);
		EXPECT_GE(Capabilities->MaxTextureArrayLayers,
			static_cast<uint32>(TextureCubeFaceCount));
		EXPECT_TRUE(std::ranges::all_of(
			Capabilities->MaxComputeWorkGroupCount,
			[](uint32 Limit) { return Limit > 0; }));
		EXPECT_TRUE(EnumHasAnyFlags(Capabilities->ColorSampleCounts,
			ERHISampleCountFlags::Samples1));
		EXPECT_TRUE(EnumHasAnyFlags(Capabilities->DepthSampleCounts,
			ERHISampleCountFlags::Samples1));
		EXPECT_TRUE(Capabilities->bSupportsGPUTimestamps);
		EXPECT_GT(Capabilities->GPUTimestampNanosecondsPerTick, 0.0);

		FRHITextureCreateDesc Texture2D = FRHITextureCreateDesc::Create2D(
			"Supported2D", 4, 4, EPixelFormat::RGBA8_UNORM);
		Texture2D.Flags = ETextureCreateFlags::ShaderResource;
		FRHITextureCreateDesc TextureCube = FRHITextureCreateDesc::CreateCube(
			"SupportedCube").SetExtent(4).SetFormat(EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource);
		FRHITextureCreateDesc Texture2DArray =
			FRHITextureCreateDesc::Create2DArray("Supported2DArray")
				.SetExtent(4, 4).SetArraySize(3)
				.SetFormat(EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
		FRHITextureCreateDesc Texture3D = FRHITextureCreateDesc::Create3D(
			"Supported3D").SetExtent(4, 4).SetDepth(4)
			.SetFormat(EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource);
		EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(Texture2D));
		EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(Texture2DArray));
		EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(Texture3D));
		EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(TextureCube));
		for (EPixelFormat Format : {EPixelFormat::R8_UNORM, EPixelFormat::RG8_UNORM,
			EPixelFormat::RGBA8_UNORM, EPixelFormat::R16_FLOAT,
			EPixelFormat::RGBA16_FLOAT})
		{
			Texture3D.SetFormat(Format).SetFlags(ETextureCreateFlags::ShaderResource);
			EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(Texture3D))
				<< static_cast<uint32>(Format);
		}
		for (EPixelFormat Format : {EPixelFormat::R8_UNORM, EPixelFormat::RGBA16_FLOAT})
		{
			Texture3D.SetFormat(Format).SetFlags(ETextureCreateFlags::Storage);
			EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(Texture3D))
				<< static_cast<uint32>(Format);
		}

		const std::array DeferredDescriptions{
			FRHITextureCreateDesc::CreateCubeArray("DeferredCubeArray")
				.SetExtent(4).SetFormat(EPixelFormat::RGBA8_UNORM),
		};
		FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
		for (const FRHITextureCreateDesc& Desc : DeferredDescriptions)
		{
			SCOPED_TRACE(static_cast<uint32>(Desc.Dimension));
			EXPECT_FALSE(GDynamicRHI->RHIIsTextureSupported(Desc));
			ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Image);
			EXPECT_FALSE(GDynamicRHI->RHICreateTexture(RHICmdList, Desc));
			EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Image));
		}

		FRHITextureCreateDesc Oversized = Texture2D;
		Oversized.SetExtent(static_cast<int32>(Capabilities->MaxTextureDimension2D + 1), 1);
		const auto TextureCreateDescResult = ValidateTextureCreateDesc(Oversized);
		ASSERT_TRUE(TextureCreateDescResult) << ToString(TextureCreateDescResult.error());
		EXPECT_FALSE(GDynamicRHI->RHIIsTextureSupported(Oversized));

		FTextureRHIRef Created2D = GDynamicRHI->RHICreateTexture(RHICmdList, Texture2D);
		FTextureRHIRef Created2DArray =
			GDynamicRHI->RHICreateTexture(RHICmdList, Texture2DArray);
		Texture3D.SetFormat(EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource);
		FTextureRHIRef Created3D = GDynamicRHI->RHICreateTexture(RHICmdList, Texture3D);
		FTextureRHIRef CreatedCube = GDynamicRHI->RHICreateTexture(RHICmdList, TextureCube);
		EXPECT_TRUE(Created2D);
		EXPECT_TRUE(Created2DArray);
		EXPECT_TRUE(Created3D);
		EXPECT_TRUE(CreatedCube);
		Created2D = nullptr;
		Created2DArray = nullptr;
		Created3D = nullptr;
		CreatedCube = nullptr;
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
	}

	TEST_F(FVulkanCreateFailureInjectionTests, PublicAsyncPipelinesShareCreationAndKeepReplayAvailable)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		struct FCoreGuard { ~FCoreGuard() { if (GDynamicRHI) RHIExit(); ShutdownTaskScheduler(); } } CoreGuard;
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentMain", "ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderError(Compiled.Error);
		for (const char* Mode : {"threaded", "inline"})
		{
			SCOPED_TRACE(Mode);
			_putenv_s("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
			std::array<FShaderRHIRef, 3> Shaders;
			for (size_t Index = 0; Index < Shaders.size(); ++Index)
			{
				const auto& Shader = Compiled.CompiledShaders[Index];
				auto Desc = FRHIShaderCreateDesc::Create(Shader.DebugName.c_str(), Shader.Frequency, *Shader.Code, Shader.Hash);
				std::string TemporaryEntryPoint = Shader.BinaryEntryPoint;
				Desc.SetEntryPoint(TemporaryEntryPoint.c_str());
				Shaders[Index] = GDynamicRHI->RHICreateShader(Desc);
				TemporaryEntryPoint.assign(TemporaryEntryPoint.size(), 'x');
				ASSERT_TRUE(Shaders[Index]);
			}
			auto Declaration = GDynamicRHI->RHICreateVertexDeclaration({});
			FGraphicsPipelineStateInitializer Graphics;
			Graphics.BoundShaders = {Shaders[0], Shaders[1]};
			Graphics.VertexDeclaration = Declaration;
			Graphics.RenderTargetLayout.NumColorRenderTargets = 1;
			Graphics.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format = EPixelFormat::RGBA8_UNORM;
			FComputePipelineStateInitializer Compute;
			Compute.ComputeShader = Shaders[2];
			FRHIPipelineCreationRequest SurvivingRequest;
			for (bool IsCompute : {false, true})
			{
				SCOPED_TRACE(IsCompute);
				const auto Request = [&] { return IsCompute
					? GDynamicRHI->RHIRequestComputePipelineState(Compute, "async")
					: GDynamicRHI->RHIRequestGraphicsPipelineState(Graphics, "async"); };
				auto Warm = Request();
				ASSERT_TRUE(Warm.Wait());
				const auto WarmResult = Warm.GetResult();
				auto& Layout = IsCompute ? Compute.PipelineLayout : Graphics.PipelineLayout;
				Layout.PushConstantRanges.push_back({
					IsCompute ? EShaderStageFlags::Compute : EShaderStageFlags::Vertex, 0, 4});
				std::promise<void> Entered, Release;
				auto EnteredFuture = Entered.get_future();
				auto Released = Release.get_future().share();
				SetVulkanPipelineCompilationHookForTest([&] { Entered.set_value(); Released.wait(); });
				const auto Before = GDynamicRHI->RHIGetPipelineCacheStatistics();
				std::vector<FRHIPipelineCreationRequest> Requests;
				Requests.push_back(Request());
				EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
				std::vector<std::future<FRHIPipelineCreationRequest>> Producers;
				for (uint32 Index = 1; Index < 16; ++Index)
					Producers.push_back(std::async(std::launch::async, Request));
				for (auto& Producer : Producers) Requests.push_back(Producer.get());
				EXPECT_TRUE(Requests[0].Cancel());
				std::promise<void> Marker;
				auto MarkerFuture = Marker.get_future();
				auto& Immediate = FRHICommandListImmediate::Get();
				Immediate.EnqueueLambda([&] { Marker.set_value(); });
				Immediate.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
				EXPECT_EQ(MarkerFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
				Layout.PushConstantRanges.clear();
				const auto Serial = GCommandListExecutor.GetLastSubmittedSerial();
				if (IsCompute) EXPECT_EQ(GDynamicRHI->RHICreateComputePipelineState("ready", Compute), WarmResult.Compute);
				else EXPECT_EQ(GDynamicRHI->RHICreateGraphicsPipelineState("ready", Graphics), WarmResult.Graphics);
				EXPECT_EQ(GCommandListExecutor.GetLastSubmittedSerial(), Serial);
				FRHICommandListFence DependencyFence;
				std::atomic<bool> Dispatched = false;
				if (IsCompute)
				{
					EXPECT_EQ(Requests[1].GetPipelineLayout()->PushConstantRanges.size(), 1u);
					Immediate.SwitchPipeline(ERHIPipeline::Compute);
					Immediate.SetComputePipelineState(Requests[1]);
					Immediate.Dispatch(1, 1, 1);
					Immediate.SwitchPipeline(ERHIPipeline::None);
					Immediate.EnqueueLambda([&] { Dispatched = true; });
					if (std::string_view(Mode) == "threaded")
					{
						Immediate.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
						DependencyFence = GCommandListExecutor.CreateFence();
						EXPECT_EQ(DependencyFence.GetState(), ERHICommandBatchState::Pending);
						EXPECT_FALSE(Dispatched.load());
					}
				}
				Release.set_value();
				for (size_t Index = 1; Index < Requests.size(); ++Index) EXPECT_TRUE(Requests[Index].Wait());
				if (IsCompute)
				{
					Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
					EXPECT_TRUE(Dispatched.load());
					if (std::string_view(Mode) == "threaded") EXPECT_TRUE(DependencyFence.TryWait());
				}
				SetVulkanPipelineCompilationHookForTest({});
				const auto After = GDynamicRHI->RHIGetPipelineCacheStatistics();
				EXPECT_EQ((IsCompute ? After.ComputePipelines.NativeCreations : After.GraphicsPipelines.NativeCreations)
					- (IsCompute ? Before.ComputePipelines.NativeCreations : Before.GraphicsPipelines.NativeCreations), 1u);
				SurvivingRequest = Requests.back();
			}
			auto* Device = static_cast<FVulkanDynamicRHI*>(GDynamicRHI)->GetDeviceForTesting();
			const auto Used = Device->GetCacheMetadataBytes();
			EXPECT_GT(Used, 0u);
			auto Reservation = Device->ReserveCacheMetadata(64ull * 1024 * 1024 - Used);
			EXPECT_THROW(Device->ReserveCacheMetadata(1), FRHIRecoverableCreationError);
			Reservation.reset();
			EXPECT_EQ(Device->GetCacheMetadataBytes(), Used);
			Shaders = {};
			Declaration = nullptr;
			auto SurvivingLayout = SurvivingRequest.GetPipelineLayout();
			GDynamicRHI->RHIStopPipelineCreation();
			EXPECT_EQ(SurvivingRequest.GetState(), ERHIPipelineRequestState::Ready);
			RHIExit();
			EXPECT_EQ(SurvivingLayout->PushConstantRanges.size(), 1u);
			EXPECT_TRUE(SurvivingRequest.GetCompletion().IsReady());
			EXPECT_EQ(SurvivingRequest.GetState(), ERHIPipelineRequestState::Canceled);
			EXPECT_FALSE(SurvivingRequest.GetResult().Graphics);
			EXPECT_FALSE(SurvivingRequest.GetResult().Compute);
		}
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		PipelineCapacityFailuresPreservePublishedEntries)
	{
		FShaderCompileOptions Options;
		Options.EntryPoints = {"VertexMain", "FragmentMain", "ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment,
			EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const auto Compiled = Compiler.Compile((std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "CreationQualification.slang").string(), Options);
		ASSERT_TRUE(Compiled) << FormatShaderError(Compiled.Error);
		ASSERT_EQ(Compiled.CompiledShaders.size(), 3u);
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		ASSERT_NE(GRHIThread, nullptr);
		std::array<FShaderRHIRef, 3> Shaders;
		for (size_t Index = 0; Index < Shaders.size(); ++Index)
		{
			const auto& Shader = Compiled.CompiledShaders[Index];
			auto Desc = FRHIShaderCreateDesc::Create(Shader.DebugName.c_str(), Shader.Frequency,
				*Shader.Code, Shader.Hash);
			Desc.SetEntryPoint(Shader.BinaryEntryPoint.c_str());
			Shaders[Index] = GDynamicRHI->RHICreateShader(Desc);
			ASSERT_TRUE(Shaders[Index]);
		}
		auto Declaration = GDynamicRHI->RHICreateVertexDeclaration({});
		ASSERT_TRUE(Declaration);
		auto* Device = static_cast<FVulkanDynamicRHI*>(GDynamicRHI)->GetDeviceForTesting();
		FGraphicsPipelineStateInitializer Graphics;
		Graphics.BoundShaders.VertexShader = Shaders[0];
		Graphics.BoundShaders.FragmentShader = Shaders[1];
		Graphics.VertexDeclaration = Declaration;
		Graphics.RenderTargetLayout.NumColorRenderTargets = 1;
		Graphics.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format = EPixelFormat::RGBA8_UNORM;
		FComputePipelineStateInitializer Compute;
		Compute.ComputeShader = Shaders[2];
		for (bool bCompute : {false, true})
		{
			SCOPED_TRACE(bCompute);
			{
				auto Access = Device->AccessPipelineCacheStatistics();
				(bCompute ? Access.Get().ComputePipelines : Access.Get().GraphicsPipelines).Capacity = 2;
			}
			const auto Create = [&](uint32 Variant) -> TRefCountPtr<FRHIResource> {
				auto G = Graphics;
				auto C = Compute;
				auto& Layout = bCompute ? C.PipelineLayout : G.PipelineLayout;
				Layout.BindingLayouts.emplace_back().BindingLayouts.emplace_back(
					bCompute ? EShaderStageFlags::Compute : EShaderStageFlags::Vertex,
					Variant, ERHIBindingType::UniformBuffer);
				TRefCountPtr<FRHIResource> Result;
				const auto Outcome = ExecuteFallibleRHICreationOperation(MakeVulkanCreationOperation([&] {
					if (bCompute)
					{
						auto Key = BuildComputePipelineStateKey(C, GDynamicRHI->RHIGetCapabilities());
						if (!Key)
							throw std::runtime_error(std::string(ToString(Key.error())));
						Result = Device->GetPipelineManager().GetOrCreateComputePipelineState(C, std::move(*Key), "BackgroundCompute");
					}
					else
					{
						auto Key = BuildGraphicsPipelineStateKey(G, GDynamicRHI->RHIGetCapabilities());
						if (!Key)
							throw std::runtime_error(std::string(ToString(Key.error())));
						Result = Device->GetPipelineManager().GetOrCreateGraphicsPipelineState(G, std::move(*Key), "BackgroundGraphics");
					}
				}));
				return !Outcome.HasError() ? Result : nullptr;
			};
			// PublicAsyncPipelinesShareCreationAndKeepReplayAvailable covers the
			// public creator's concurrency. Exercise cache admission serially here.
			auto Warm = Create(0);
			ASSERT_TRUE(Warm);
			auto Cold = Create(1);
			ASSERT_TRUE(Cold);
			EXPECT_EQ(Create(1), Cold);
			auto Hit = Create(0);
			auto& Commands = GCommandListExecutor.GetImmediateCommandList();
			EXPECT_EQ(Hit, Warm);
			EXPECT_FALSE(Create(2));
			const auto* OldCold = Cold.GetReference();
			Cold = nullptr;
			ArmVulkanCreateFailure(EVulkanCreateFailurePoint::PipelineLayout);
			EXPECT_FALSE(Create(2));
			auto Restored = Create(1);
			EXPECT_EQ(Restored.GetReference(), OldCold);
			Restored = nullptr;
			auto Replacement = Create(2);
			EXPECT_TRUE(Replacement);
			EXPECT_EQ(Create(0), Warm);
			const auto Stats = Device->GetPipelineCacheStatistics();
			const auto& Cache = bCompute ? Stats.ComputePipelines : Stats.GraphicsPipelines;
			EXPECT_EQ(Cache.Occupancy, 2u);
			EXPECT_EQ(Cache.NativeCreations, 3u);
			EXPECT_EQ(Cache.FailedCandidates, 2u);
			EXPECT_EQ(Cache.Evictions, 1u);
			Warm = nullptr;
			Hit = nullptr;
			Replacement = nullptr;
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		}
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		RuntimeFactoriesReturnNullThenRecoverOnTheSameRHIThread)
	{
		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		ResetVulkanMemoryBaselineStatistics();
		ASSERT_NE(GRHIThread, nullptr);
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		struct FRenderingThreadScope
		{
			FRenderingThreadScope() { InitRenderingThread(); }
			~FRenderingThreadScope() { ShutdownRenderingThread(); }
		} RenderingThreadScope;

		FRHICommandListImmediate& RHICmdList =
			FRHICommandListImmediate::Get();

		const FRHIBufferCreateDesc BufferDesc = FRHIBufferCreateDesc::Create(
			"RecoverableBuffer", 256, 16,
			EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);
		EXPECT_FALSE(GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc));
		FBufferRHIRef Buffer =
			GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc);
		ASSERT_TRUE(Buffer);

		FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create2D(
			"RecoverableTexture", 4, 4, EPixelFormat::RGBA8_UNORM);
		TextureDesc.Flags = ETextureCreateFlags::ShaderResource;
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Image);
		EXPECT_FALSE(GDynamicRHI->RHICreateTexture(RHICmdList, TextureDesc));
		EXPECT_EQ(GDynamicRHI->RHIGetMemoryStatistics().Classes[
			static_cast<uint32>(ERHIMemoryAllocationClass::DeviceLocal)]
			.AllocationFailureCount, 2u);
		FTextureRHIRef Texture =
			GDynamicRHI->RHICreateTexture(RHICmdList, TextureDesc);
		ASSERT_TRUE(Texture);
		const FRHITextureViewDesc TextureViewDesc = MakeDefaultTextureViewDesc(
			*Texture, ERHITextureViewUsage::Sampled);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::ImageView);
		EXPECT_FALSE(GDynamicRHI->RHICreateTextureView(Texture, TextureViewDesc));
		FTextureViewRHIRef TextureView = GDynamicRHI->RHICreateTextureView(
			Texture, TextureViewDesc);
		ASSERT_TRUE(TextureView);
		RHICmdList.TransitionTextures(std::array{FRHITextureTransition{
			Texture, {ERHITextureAspect::Color, 0, 1, 0, 1},
			ERHIAccess::Discard, ERHIAccess::GraphicsShaderRead}});
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::SubmitToGPU);

		FRHISamplerDesc SamplerDesc;
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Sampler);
		EXPECT_FALSE(GDynamicRHI->RHICreateSampler(SamplerDesc));
		TRefCountPtr<FRHISampler> Sampler =
			GDynamicRHI->RHICreateSampler(SamplerDesc);
		ASSERT_TRUE(Sampler);

		FVertexDeclarationElementList Elements;
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::VertexDeclaration);
		EXPECT_FALSE(GDynamicRHI->RHICreateVertexDeclaration(Elements));
		FVertexDeclarationRHIRef VertexDeclaration =
			GDynamicRHI->RHICreateVertexDeclaration(Elements);
		ASSERT_TRUE(VertexDeclaration);
		bool bRHIThreadLocalFailureReturnedNull = false;
		FVertexDeclarationRHIRef RHIThreadLocalVertexDeclaration;
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::VertexDeclaration);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			bRHIThreadLocalFailureReturnedNull =
				!GDynamicRHI->RHICreateVertexDeclaration(Elements);
			RHIThreadLocalVertexDeclaration =
				GDynamicRHI->RHICreateVertexDeclaration(Elements);
		});
		EXPECT_TRUE(bRHIThreadLocalFailureReturnedNull);
		ASSERT_TRUE(RHIThreadLocalVertexDeclaration);

		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "RecoverableResourceFactories.slang";
		FShaderCompileOptions CompileOptions;
		CompileOptions.EntryPoints = {
			"VertexMain", "FragmentMain", "FragmentMrtMain"};
		CompileOptions.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment,
			EShaderFrequency::Fragment};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput CompileOutput =
			Compiler.Compile(ShaderPath.string(), CompileOptions);
		ASSERT_TRUE(CompileOutput) << FormatShaderError(CompileOutput.Error);
		ASSERT_EQ(CompileOutput.CompiledShaders.size(), 3u);

		auto MakeCreateDesc = [](const FCompiledShader& CompiledShader) {
			FRHIShaderCreateDesc Desc = FRHIShaderCreateDesc::Create(
				CompiledShader.DebugName.c_str(), CompiledShader.Frequency,
				*CompiledShader.Code, CompiledShader.Hash);
			Desc.SetEntryPoint(CompiledShader.BinaryEntryPoint.c_str());
			return Desc;
		};
		const FRHIShaderCreateDesc VertexShaderDesc =
			MakeCreateDesc(CompileOutput.CompiledShaders[0]);
		const FRHIShaderCreateDesc FragmentShaderDesc =
			MakeCreateDesc(CompileOutput.CompiledShaders[1]);
		const FRHIShaderCreateDesc MrtFragmentShaderDesc =
			MakeCreateDesc(CompileOutput.CompiledShaders[2]);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::ShaderModule);
		EXPECT_FALSE(GDynamicRHI->RHICreateShader(VertexShaderDesc));
		FShaderRHIRef VertexShader =
			GDynamicRHI->RHICreateShader(VertexShaderDesc);
		FShaderRHIRef FragmentShader =
			GDynamicRHI->RHICreateShader(FragmentShaderDesc);
		FShaderRHIRef MrtFragmentShader =
			GDynamicRHI->RHICreateShader(MrtFragmentShaderDesc);
		ASSERT_TRUE(VertexShader);
		ASSERT_TRUE(FragmentShader);
		ASSERT_TRUE(MrtFragmentShader);

		FRHIRenderTargetLayout RenderTargetLayout;
		RenderTargetLayout.NumColorRenderTargets = 1;
		auto& ColorAttachment =
			RenderTargetLayout.ColorAttachments[0].RenderTarget;
		ColorAttachment.Format = EPixelFormat::RGBA8_UNORM;
		ColorAttachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
		ColorAttachment.StoreAction = ERHIRenderTargetStoreAction::Store;
		ColorAttachment.InitialLayout = ERHITextureLayout::Undefined;
		ColorAttachment.InitialAccess = ERHIAccess::None;
		ColorAttachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
		ColorAttachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
		FGraphicsPipelineStateInitializer Initializer;
		Initializer.RenderTargetLayout = RenderTargetLayout;
		Initializer.BoundShaders.VertexShader = VertexShader;
		Initializer.BoundShaders.FragmentShader = FragmentShader;
		Initializer.VertexDeclaration = VertexDeclaration;
		auto& PipelineBindings =
			Initializer.PipelineLayout.BindingLayouts.emplace_back().BindingLayouts;
		PipelineBindings.emplace_back(
			EShaderStageFlags::Vertex, 0, ERHIBindingType::Texture);
		PipelineBindings.emplace_back(
			EShaderStageFlags::Fragment, 1, ERHIBindingType::Sampler, 2);

		const FName PipelineName("RecoverableGraphicsPipeline");
		const FVulkanGraphicsPipelineTestStats PipelineStatsBefore =
			GetVulkanGraphicsPipelineTestStats();
		const FVulkanStructuralCacheTestStats StructuralStatsBefore =
			GetVulkanStructuralCacheTestStats();
		const FRHIPipelineCacheStatistics CacheStatsBefore =
			GDynamicRHI->RHIGetPipelineCacheStatistics();
		// Timing capture is bounded and opt-in; this fixture validates boundaries only.
		struct FCreationCaptureScope
		{
			FCreationCaptureScope() { BeginVulkanCreationTimingCapture(256); }
			~FCreationCaptureScope()
			{
				uint64 Dropped = 0;
				const auto Samples = EndVulkanCreationTimingCapture(Dropped);
				EXPECT_EQ(Dropped, 0u);
				EXPECT_FALSE(Samples.empty());
				bool bNative = false;
				bool bReady = false;
				bool bFailure = false;
				for (const auto& Sample : Samples)
				{
					EXPECT_GT(Sample.RequestId, 0u);
					EXPECT_GE(Sample.Returned, Sample.Entry);
					if (Sample.Scheduled)
					{
						EXPECT_GE(Sample.Scheduled, Sample.Entry);
						EXPECT_GE(Sample.BodyStart, Sample.Scheduled);
						EXPECT_GE(Sample.BodyEnd, Sample.BodyStart);
						EXPECT_GE(Sample.Returned, Sample.BodyEnd);
					}
					if (Sample.NativeStart)
					{
						bNative = true;
						EXPECT_GE(Sample.NativeStart, Sample.BodyStart);
						EXPECT_GE(Sample.NativeEnd, Sample.NativeStart);
						EXPECT_GE(Sample.BodyEnd, Sample.NativeEnd);
					}
					bReady |= Sample.bSucceeded && !Sample.NativeStart;
					bFailure |= !Sample.bSucceeded;
				}
				EXPECT_TRUE(bNative);
				EXPECT_TRUE(bReady);
				EXPECT_TRUE(bFailure);
			}
		} CreationCapture;
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::RenderPass);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			PipelineName, Initializer));
		EXPECT_EQ(GetVulkanStructuralCacheTestStats().RenderPassEntryCount,
			StructuralStatsBefore.RenderPassEntryCount);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::DescriptorSetLayout);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			PipelineName, Initializer));
		const FVulkanStructuralCacheTestStats StatsAfterDescriptorFailure =
			GetVulkanStructuralCacheTestStats();
		EXPECT_EQ(StatsAfterDescriptorFailure.RenderPassEntryCount,
			StructuralStatsBefore.RenderPassEntryCount + 1);
		EXPECT_EQ(StatsAfterDescriptorFailure.DescriptorSetLayoutEntryCount,
			StructuralStatsBefore.DescriptorSetLayoutEntryCount);
		EXPECT_EQ(StatsAfterDescriptorFailure.PipelineLayoutEntryCount,
			StructuralStatsBefore.PipelineLayoutEntryCount);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::PipelineLayout);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			PipelineName, Initializer));
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::GraphicsPipeline);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			PipelineName, Initializer));
		FGraphicsPipelineStateRHIRef Pipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, Initializer);
		ASSERT_TRUE(Pipeline);
		FGraphicsPipelineStateRHIRef SameNamePipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, Initializer);
		ASSERT_TRUE(SameNamePipeline);
		EXPECT_EQ(Pipeline.GetReference(), SameNamePipeline.GetReference());
		EXPECT_EQ(Pipeline->GetRefCount(), 3u);
		EXPECT_EQ(SameNamePipeline->GetRefCount(), 3u);

		FGraphicsPipelineStateInitializer ChangedInitializer = Initializer;
		ChangedInitializer.RasterizerState.CullMode = ERHICullMode::None;
		FGraphicsPipelineStateRHIRef ChangedSameNamePipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, ChangedInitializer);
		ASSERT_TRUE(ChangedSameNamePipeline);
		EXPECT_NE(Pipeline.GetReference(), ChangedSameNamePipeline.GetReference());
		EXPECT_EQ(ChangedSameNamePipeline->GetRefCount(), 2u);

		FGraphicsPipelineStateInitializer PositiveZeroInitializer = Initializer;
		PositiveZeroInitializer.RasterizerState.bEnableDepthBias = true;
		PositiveZeroInitializer.RasterizerState.DepthBiasConstantFactor = 0.0f;
		PositiveZeroInitializer.RasterizerState.DepthBiasClamp = 0.0f;
		PositiveZeroInitializer.RasterizerState.DepthBiasSlopeFactor = 0.0f;
		FGraphicsPipelineStateInitializer NegativeZeroInitializer =
			PositiveZeroInitializer;
		NegativeZeroInitializer.RasterizerState.DepthBiasConstantFactor = -0.0f;
		NegativeZeroInitializer.RasterizerState.DepthBiasClamp = -0.0f;
		NegativeZeroInitializer.RasterizerState.DepthBiasSlopeFactor = -0.0f;
		FGraphicsPipelineStateRHIRef PositiveZeroPipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				"RecoverableGraphicsPipeline_PositiveZero", PositiveZeroInitializer);
		FGraphicsPipelineStateRHIRef NegativeZeroPipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				"RecoverableGraphicsPipeline_NegativeZero", NegativeZeroInitializer);
		ASSERT_TRUE(PositiveZeroPipeline && NegativeZeroPipeline);
		EXPECT_EQ(PositiveZeroPipeline.GetReference(),
			NegativeZeroPipeline.GetReference());
		FGraphicsPipelineStateInitializer NonzeroBiasInitializer =
			PositiveZeroInitializer;
		NonzeroBiasInitializer.RasterizerState.DepthBiasConstantFactor = 1.25f;
		NonzeroBiasInitializer.RasterizerState.DepthBiasClamp = 4.0f;
		NonzeroBiasInitializer.RasterizerState.DepthBiasSlopeFactor = 1.75f;
		FGraphicsPipelineStateRHIRef NonzeroBiasPipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				"RecoverableGraphicsPipeline_NonzeroBias", NonzeroBiasInitializer);
		ASSERT_TRUE(NonzeroBiasPipeline);
		EXPECT_EQ(PositiveZeroPipeline.GetReference(),
			NonzeroBiasPipeline.GetReference());

		std::vector<FGraphicsPipelineStateRHIRef> StatePipelines;
		auto CreateStatePipeline = [&](FGraphicsPipelineStateInitializer State,
			std::string_view Suffix) {
			FGraphicsPipelineStateRHIRef StatePipeline =
				GDynamicRHI->RHICreateGraphicsPipelineState(
					FName(std::format("RecoverableGraphicsPipeline_{}", Suffix)),
					State);
			EXPECT_TRUE(StatePipeline);
			if (StatePipeline)
				StatePipelines.push_back(std::move(StatePipeline));
		};
		FGraphicsPipelineStateInitializer WireframeInitializer = Initializer;
		WireframeInitializer.RasterizerState.PolygonMode = ERHIPolygonMode::Line;
		CreateStatePipeline(WireframeInitializer, "Wireframe");
		FGraphicsPipelineStateInitializer CounterClockwiseInitializer = Initializer;
		CounterClockwiseInitializer.RasterizerState.FrontFace =
			ERHIFrontFace::CounterClockwise;
		CreateStatePipeline(CounterClockwiseInitializer, "CounterClockwise");
		FGraphicsPipelineStateInitializer DepthInitializer = Initializer;
		DepthInitializer.RenderTargetLayout.bHasDepthStencil = true;
		auto& DepthAttachment =
			DepthInitializer.RenderTargetLayout.DepthStencilAttachment;
		DepthAttachment.Format = EPixelFormat::D32;
		DepthAttachment.InitialLayout = ERHITextureLayout::Undefined;
		DepthAttachment.InitialAccess = ERHIAccess::None;
		DepthAttachment.FinalLayout = ERHITextureLayout::DepthStencilAttachment;
		DepthAttachment.FinalAccess = ERHIAccess::DepthStencilReadWrite;
		DepthInitializer.DepthStencilState.bEnableTest = true;
		DepthInitializer.DepthStencilState.bEnableWrite = true;
		CreateStatePipeline(DepthInitializer, "Depth");
		FGraphicsPipelineStateInitializer BlendInitializer = Initializer;
		BlendInitializer.ColorBlendStates[0] = FRHIColorBlendState::StraightAlpha();
		CreateStatePipeline(BlendInitializer, "StraightAlpha");
		FRHITextureCreateDesc MrtDepthDesc = FRHITextureCreateDesc::Create2D(
			"MrtStencilValidationDepth", 8, 8, EPixelFormat::D24S8)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable);
		if (!GDynamicRHI->RHIIsTextureSupported(MrtDepthDesc))
		{
			MrtDepthDesc.Format = EPixelFormat::D32S8;
		}
		ASSERT_TRUE(GDynamicRHI->RHIIsTextureSupported(MrtDepthDesc));
		FGraphicsPipelineStateInitializer MrtStencilInitializer = Initializer;
		MrtStencilInitializer.RenderTargetLayout.NumColorRenderTargets = 2;
		MrtStencilInitializer.RenderTargetLayout.ColorAttachments[1] =
			MrtStencilInitializer.RenderTargetLayout.ColorAttachments[0];
		MrtStencilInitializer.RenderTargetLayout.bHasDepthStencil = true;
		auto& MrtDepthAttachment =
			MrtStencilInitializer.RenderTargetLayout.DepthStencilAttachment;
		MrtDepthAttachment.Format = MrtDepthDesc.Format;
		MrtDepthAttachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
		MrtDepthAttachment.StoreAction = ERHIRenderTargetStoreAction::Store;
		MrtDepthAttachment.StencilLoadAction = ERHIRenderTargetLoadAction::Clear;
		MrtDepthAttachment.StencilStoreAction = ERHIRenderTargetStoreAction::Store;
		MrtDepthAttachment.InitialLayout = ERHITextureLayout::Undefined;
		MrtDepthAttachment.InitialAccess = ERHIAccess::None;
		MrtDepthAttachment.FinalLayout = ERHITextureLayout::DepthStencilAttachment;
		MrtDepthAttachment.FinalAccess = ERHIAccess::DepthStencilReadWrite;
		MrtStencilInitializer.DepthStencilState.bEnableTest = true;
		MrtStencilInitializer.DepthStencilState.bEnableWrite = true;
		MrtStencilInitializer.DepthStencilState.bEnableStencil = true;
		MrtStencilInitializer.ColorBlendStates[1] =
			FRHIColorBlendState::StraightAlpha();
		MrtStencilInitializer.BoundShaders.FragmentShader = MrtFragmentShader;
		FGraphicsPipelineStateRHIRef MrtStencilPipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				"RecoverableGraphicsPipeline_MrtStencil", MrtStencilInitializer);
		ASSERT_TRUE(MrtStencilPipeline);
		StatePipelines.push_back(MrtStencilPipeline);

		FGraphicsPipelineStateInitializer TwoSetInitializer = Initializer;
		TwoSetInitializer.PipelineLayout.BindingLayouts.emplace_back()
			.BindingLayouts.emplace_back(
				EShaderStageFlags::Fragment, 1, ERHIBindingType::Sampler);
		const FVulkanStructuralCacheTestStats StatsBeforeDependentLayoutFailure =
			GetVulkanStructuralCacheTestStats();
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::DescriptorSetLayout);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			"DependentDescriptorFailure", TwoSetInitializer));
		const FVulkanStructuralCacheTestStats StatsAfterDependentLayoutFailure =
			GetVulkanStructuralCacheTestStats();
		EXPECT_EQ(StatsAfterDependentLayoutFailure.DescriptorSetLayoutEntryCount,
			StatsBeforeDependentLayoutFailure.DescriptorSetLayoutEntryCount);
		EXPECT_EQ(StatsAfterDependentLayoutFailure.PipelineLayoutEntryCount,
			StatsBeforeDependentLayoutFailure.PipelineLayoutEntryCount);
		FGraphicsPipelineStateRHIRef TwoSetPipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				"RecoveredDependentDescriptor", TwoSetInitializer);
		ASSERT_TRUE(TwoSetPipeline);

		FGraphicsPipelineStateInitializer InvalidInitializer = Initializer;
		InvalidInitializer.RasterizerState.FrontFace = ERHIFrontFace::Count;
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			"InvalidGraphicsPipeline", InvalidInitializer));
		const FVulkanGraphicsPipelineTestStats PipelineStatsAfterCreation =
			GetVulkanGraphicsPipelineTestStats();
		EXPECT_EQ(
			PipelineStatsAfterCreation.CommittedPipelineCount,
			PipelineStatsBefore.CommittedPipelineCount + 9);
		EXPECT_EQ(
			PipelineStatsAfterCreation.CreatedPipelineLayoutCount,
			PipelineStatsBefore.CreatedPipelineLayoutCount + 10);
		EXPECT_EQ(
			PipelineStatsAfterCreation.RolledBackPipelineLayoutCount,
			PipelineStatsBefore.RolledBackPipelineLayoutCount + 1);
		const FVulkanStructuralCacheTestStats StatsAfterPipelineCreation =
			GetVulkanStructuralCacheTestStats();
		EXPECT_EQ(StatsAfterPipelineCreation.DescriptorSetLayoutEntryCount,
			StructuralStatsBefore.DescriptorSetLayoutEntryCount + 2);
		EXPECT_EQ(StatsAfterPipelineCreation.PipelineLayoutEntryCount,
			StructuralStatsBefore.PipelineLayoutEntryCount + 2);
		const FRHIPipelineCacheStatistics CacheStatsAfterCreation =
			GDynamicRHI->RHIGetPipelineCacheStatistics();
		EXPECT_GE(CacheStatsAfterCreation.GraphicsPipelines.Hits,
			CacheStatsBefore.GraphicsPipelines.Hits + 1);
		EXPECT_EQ(CacheStatsAfterCreation.GraphicsPipelines.NativeCreations,
			CacheStatsBefore.GraphicsPipelines.NativeCreations + 9);
		EXPECT_EQ(CacheStatsAfterCreation.GraphicsPipelines.Occupancy,
			CacheStatsBefore.GraphicsPipelines.Occupancy + 9);
		EXPECT_GE(CacheStatsAfterCreation.GraphicsPipelines.FailedCandidates,
			CacheStatsBefore.GraphicsPipelines.FailedCandidates + 4);
		EXPECT_EQ(CacheStatsAfterCreation.GraphicsPipelines.Capacity, 2048u);
		EXPECT_EQ(CacheStatsAfterCreation.StructuralLayouts.Capacity, 256u);
		EXPECT_EQ(CacheStatsAfterCreation.DescriptorSnapshots.Capacity, 512u);
		EXPECT_EQ(CacheStatsAfterCreation.DescriptorValueCapacity, 8192u);

		FRHITextureCreateDesc RenderTargetDesc = FRHITextureCreateDesc::Create2D(
			"RecoverableFramebufferTexture", 8, 8, EPixelFormat::RGBA8_UNORM);
		RenderTargetDesc.Flags = ETextureCreateFlags::RenderTargetable
			| ETextureCreateFlags::ShaderResource;
		FTextureRHIRef RenderTarget =
			GDynamicRHI->RHICreateTexture(RHICmdList, RenderTargetDesc);
		ASSERT_TRUE(RenderTarget);
		FRHIRenderPassInfo PassInfo;
		PassInfo.RenderTargetLayout = RenderTargetLayout;
		PassInfo.ColorRenderTargets[0] = RenderTarget;
		PassInfo.ColorClearValues[0] = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		const FVulkanStructuralCacheTestStats StatsBeforeFramebuffer =
			GetVulkanStructuralCacheTestStats();
		auto ExpectFramebufferFailure = [&](EVulkanCreateFailurePoint FailurePoint) {
			ArmVulkanCreateFailure(FailurePoint);
			bool bFailed = false;
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				try
				{
					auto* Context = static_cast<FVulkanCommandListContext*>(
						GDynamicRHI->RHIGetDefaultContext());
					Context->RHIBeginRenderPass(PassInfo, "InjectedFramebufferFailure");
					Context->RHIEndRenderPass();
				}
				catch (...)
				{
					bFailed = true;
				}
			});
			EXPECT_TRUE(bFailed);
			EXPECT_EQ(GetVulkanStructuralCacheTestStats().FramebufferEntryCount,
				StatsBeforeFramebuffer.FramebufferEntryCount);
		};
		ExpectFramebufferFailure(EVulkanCreateFailurePoint::FramebufferImageView);
		ExpectFramebufferFailure(EVulkanCreateFailurePoint::Framebuffer);
		const FVulkanStructuralCacheTestStats StatsAfterFramebufferFailure =
			GetVulkanStructuralCacheTestStats();
		EXPECT_EQ(
			StatsAfterFramebufferFailure.CreatedFramebufferViewCount
				- StatsBeforeFramebuffer.CreatedFramebufferViewCount,
			StatsAfterFramebufferFailure.ReleasedFramebufferViewCount
				- StatsBeforeFramebuffer.ReleasedFramebufferViewCount);
		uint64 DescriptorSubmissionToken = 0;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			auto* Context = static_cast<FVulkanCommandListContext*>(
				GDynamicRHI->RHIGetDefaultContext());
			Context->RHIBeginRenderPass(PassInfo, "RecoveredFramebuffer");
			Context->RHIEndRenderPass();
		});
		EXPECT_EQ(GetVulkanStructuralCacheTestStats().FramebufferEntryCount,
			StatsBeforeFramebuffer.FramebufferEntryCount + 1);
		auto DrawDescriptorArray = [&](bool& bFailed) {
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				auto* Context = static_cast<FVulkanCommandListContext*>(
					GDynamicRHI->RHIGetDefaultContext());
				Context->RHIBeginRenderPass(PassInfo, "DescriptorArrayDraw");
				Context->RHISetGraphicsPipelineState(*Pipeline);
				Context->RHISetViewport(0.0f, 0.0f, 0.0f, 8.0f, 8.0f, 1.0f);
				FTextureViewRHIRef TransientTextureView =
					GDynamicRHI->RHICreateTextureView(Texture,
						MakeDefaultTextureViewDesc(*Texture,
							ERHITextureViewUsage::Sampled));
				check(TransientTextureView);
				std::array<FRHIShaderParameterResource, 1> TextureParameters{
					FRHIShaderParameterResource{.Resource = TransientTextureView.GetReference(),
						.SetIndex = 0, .BindingIndex = 0, .ArrayElement = 0,
						.Type = ERHIBindingType::Texture}};
				Context->RHISetShaderParameters(VertexShader, TextureParameters);
				TransientTextureView = nullptr;
				std::array<FRHIShaderParameterResource, 2> SamplerParameters{
					FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
						.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 0,
						.Type = ERHIBindingType::Sampler},
					FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
						.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 1,
						.Type = ERHIBindingType::Sampler}};
				Context->RHISetShaderParameters(FragmentShader, SamplerParameters);
				try
				{
					Context->RHIDraw({.VertexCount = 3});
				}
				catch (...)
				{
					Context->RHIEndRenderPass();
					bFailed = true;
					return;
				}
				Context->RHIEndRenderPass();
			});
		};
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::DescriptorPool);
		bool bDescriptorPoolFailed = false;
		DrawDescriptorArray(bDescriptorPoolFailed);
		EXPECT_TRUE(bDescriptorPoolFailed);
		EXPECT_EQ(GetVulkanMemoryBaselineStatistics().DescriptorPoolCount, 0u);
		bool bDescriptorPoolRecoveryFailed = false;
		DrawDescriptorArray(bDescriptorPoolRecoveryFailed);
		EXPECT_FALSE(bDescriptorPoolRecoveryFailed);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			DescriptorSubmissionToken =
				SubmitAndRetireDescriptorPoolsForTesting();
		});
		const FVulkanMemoryBaselineStatistics DescriptorBaseline =
			GetVulkanMemoryBaselineStatistics();
		EXPECT_GT(DescriptorBaseline.DescriptorPoolCount, 0u);
		EXPECT_GT(DescriptorBaseline.DescriptorPoolSetCapacity, 0u);
		EXPECT_GT(DescriptorBaseline.DescriptorPeakAllocatedSetCount, 0u);
		FVulkanBackendPoolTestStats DescriptorPoolStats;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			DescriptorPoolStats = GetVulkanBackendPoolTestStats();
		});
		EXPECT_NE(std::ranges::find(DescriptorPoolStats.DescriptorPoolTokens,
			DescriptorSubmissionToken),
			DescriptorPoolStats.DescriptorPoolTokens.end());
		FRHITextureCreateDesc MrtColorDesc = FRHITextureCreateDesc::Create2D(
			"MrtStencilValidationColor", 8, 8, EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::CPUReadback);
		FTextureRHIRef MrtColor0 =
			GDynamicRHI->RHICreateTexture(RHICmdList, MrtColorDesc);
		MrtColorDesc.DebugName = "MrtStencilValidationColor1";
		FTextureRHIRef MrtColor1 =
			GDynamicRHI->RHICreateTexture(RHICmdList, MrtColorDesc);
		FTextureRHIRef MrtDepth =
			GDynamicRHI->RHICreateTexture(RHICmdList, MrtDepthDesc);
		ASSERT_TRUE(MrtColor0 && MrtColor1 && MrtDepth);
		FRHIRenderPassInfo MrtPassInfo;
		MrtPassInfo.RenderTargetLayout = MrtStencilInitializer.RenderTargetLayout;
		MrtPassInfo.ColorRenderTargets[0] = MrtColor0;
		MrtPassInfo.ColorRenderTargets[1] = MrtColor1;
		MrtPassInfo.DepthStencilRenderTarget = MrtDepth;
		MrtPassInfo.ColorClearValues[0] = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		MrtPassInfo.ColorClearValues[1] = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		Durin::FByteBuffer MrtPixels;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			auto* Context = static_cast<FVulkanCommandListContext*>(
				GDynamicRHI->RHIGetDefaultContext());
			Context->RHIBeginRenderPass(MrtPassInfo, "MrtStencilDraw");
			Context->RHISetViewport(0.0f, 0.0f, 0.0f, 8.0f, 8.0f, 1.0f);
			Context->RHISetGraphicsPipelineState(*MrtStencilPipeline);
			std::array<FRHIShaderParameterResource, 3> Parameters{
				FRHIShaderParameterResource{.Resource = TextureView.GetReference(),
					.SetIndex = 0, .BindingIndex = 0, .ArrayElement = 0,
					.Type = ERHIBindingType::Texture},
				FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
					.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 0,
					.Type = ERHIBindingType::Sampler},
				FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
					.SetIndex = 0, .BindingIndex = 1, .ArrayElement = 1,
					.Type = ERHIBindingType::Sampler}};
			Context->RHISetShaderParameters(VertexShader,
				std::span(Parameters).first(1));
			Context->RHISetShaderParameters(MrtFragmentShader,
				std::span(Parameters).subspan(1));
			Context->RHIDraw({.VertexCount = 3});
			Context->RHIEndRenderPass();
		});
		ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
			RHICmdList, MrtColor0, 0, 0, MrtPixels));
		EXPECT_EQ(MrtPixels.size(), 8u * 8u * 4u);

		StatePipelines.clear();
		PositiveZeroPipeline = nullptr;
		NegativeZeroPipeline = nullptr;
		TwoSetPipeline = nullptr;
		ChangedSameNamePipeline = nullptr;
		SameNamePipeline = nullptr;
		Pipeline = nullptr;
		FragmentShader = nullptr;
		MrtFragmentShader = nullptr;
		VertexShader = nullptr;
		RHIThreadLocalVertexDeclaration = nullptr;
		VertexDeclaration = nullptr;
		Sampler = nullptr;
		TextureView = nullptr;
		Texture = nullptr;
		RenderTarget = nullptr;
		MrtDepth = nullptr;
		MrtColor1 = nullptr;
		MrtColor0 = nullptr;
		Buffer = nullptr;
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		GDynamicRHI->RHIResetPipelineCacheStatistics();
		const FRHIPipelineCacheStatistics ResetCacheStats =
			GDynamicRHI->RHIGetPipelineCacheStatistics();
		EXPECT_EQ(ResetCacheStats.GraphicsPipelines.Hits, 0u);
		EXPECT_EQ(ResetCacheStats.GraphicsPipelines.NativeCreations, 0u);
		EXPECT_EQ(ResetCacheStats.GraphicsPipelines.Occupancy,
			CacheStatsAfterCreation.GraphicsPipelines.Occupancy);
		const FVulkanGraphicsPipelineTestStats PipelineStatsAfterRelease =
			GetVulkanGraphicsPipelineTestStats();
		EXPECT_EQ(
			PipelineStatsAfterRelease.DestroyedPipelineCount,
			PipelineStatsBefore.DestroyedPipelineCount);
	}

	TEST_F(FVulkanPublicRHIConformanceTests,
		ViewportOutputCandidatesFailAtomicallyAndRecover)
	{
		class FTestApplication final : public FGenericApplication
		{
		public:
			explicit FTestApplication(std::vector<std::shared_ptr<FGenericWindow>> InWindows)
				: Windows(std::move(InWindows))
			{
			}

			auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle)
				-> std::shared_ptr<FGenericWindow> override
			{
				const auto It = std::ranges::find_if(Windows,
					[InNativeWindowHandle](const std::shared_ptr<FGenericWindow>& Window) {
						return Window && Window->GetOSNativeWindowHandle() == InNativeWindowHandle;
					});
				return It != Windows.end() ? *It : nullptr;
			}

			std::vector<std::shared_ptr<FGenericWindow>> Windows;
		};

		struct FApplicationCoreScope
		{
			FApplicationCoreScope() { InitializeApplicationCore(); }
			~FApplicationCoreScope()
			{
				GApp = nullptr;
				ShutdownApplicationCore();
			}
		} ApplicationCoreScope;
		std::shared_ptr<FGenericWindow> Window = MakePlatformWindow();
		auto WindowDefinition = std::make_shared<FGenericWindowDefinition>();
		WindowDefinition->XDesiredPositionOnScreen = 0.0f;
		WindowDefinition->YDesiredPositionOnScreen = 0.0f;
		WindowDefinition->WidthDesiredOnScreen = 64.0f;
		WindowDefinition->HeightDesiredOnScreen = 64.0f;
		WindowDefinition->Title = "Vulkan viewport transaction test";
		Window->Initialize(WindowDefinition);
		ASSERT_NE(Window->GetOSNativeWindowHandle(), nullptr);
		std::shared_ptr<FGenericWindow> DetachedWindow = MakePlatformWindow();
		auto DetachedDefinition = std::make_shared<FGenericWindowDefinition>(*WindowDefinition);
		DetachedDefinition->XDesiredPositionOnScreen = 96.0f;
		DetachedDefinition->Title = "Vulkan detached viewport transaction test";
		DetachedWindow->Initialize(DetachedDefinition);
		ASSERT_NE(DetachedWindow->GetOSNativeWindowHandle(), nullptr);
		GApp = std::make_shared<FTestApplication>(
			std::vector<std::shared_ptr<FGenericWindow>>{Window, DetachedWindow});
		ASSERT_EQ(GApp->FindWindowByNativeWindowHandle(
			Window->GetOSNativeWindowHandle()), Window);

		_putenv_s("DURIN_VULKAN_VALIDATION", "on");
		const FRHIInitializationContext InitializationContext =
			FRHIInitializationContext::Presentation({
			.NativeWindowHandle = Window->GetOSNativeWindowHandle()});
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Surface);
		EXPECT_FALSE(RHIInit(InitializationContext));
		EXPECT_EQ(GDynamicRHI, nullptr);
		ExpectVulkanModuleUnloaded();

		ASSERT_TRUE(RHIInit(InitializationContext));
		ASSERT_NE(GRHIThread, nullptr);
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		struct FRenderingThreadScope
		{
			FRenderingThreadScope() { InitRenderingThread(); }
			~FRenderingThreadScope() { ShutdownRenderingThread(); }
		} RenderingThreadScope;

		FRHIViewportCreateInfo MainCreateInfo{
			.NativeWindowHandle = Window->GetOSNativeWindowHandle(),
			.SizeX = 64,
			.SizeY = 64,
			.PreferredPixelFormat = EPixelFormat::SBGRA8_UNORM,
			.PresentationPolicy = EViewportPresentationPolicy::FramePaced,
			.bAdoptInitializationPresentationCandidate = true};
		FRHIViewportCreateInfo MismatchedCreateInfo = MainCreateInfo;
		MismatchedCreateInfo.NativeWindowHandle =
			DetachedWindow->GetOSNativeWindowHandle();
		EXPECT_FALSE(GDynamicRHI->RHICreateViewport(MismatchedCreateInfo));

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain);
		TRefCountPtr<FRHIViewport> Viewport =
			GDynamicRHI->RHICreateViewport(MainCreateInfo);
		ASSERT_TRUE(Viewport);
		EXPECT_FALSE(GDynamicRHI->RHICreateViewport(MainCreateInfo));

		auto* VulkanViewport = static_cast<FVulkanViewport*>(Viewport.GetReference());
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			EXPECT_FALSE(VulkanViewport->HasAvailableOutput());
		});
		bool bInitialBackBufferWasNull = false;
		ENQUEUE_RENDER_COMMAND(CheckUnavailableVulkanViewport)(
			[Viewport, &bInitialBackBufferWasNull](FRHICommandListImmediate&) {
				bInitialBackBufferWasNull =
					!GDynamicRHI->RHIGetViewportBackBuffer(Viewport);
			});
		FlushRenderingCommands();
		EXPECT_TRUE(bInitialBackBufferWasNull);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->BeginDrawing();
		});
		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain));
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
		});
		vk::SwapchainKHR FirstSwapchain = VK_NULL_HANDLE;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			ASSERT_NE(VulkanViewport->GetSwapchain(), nullptr);
			FirstSwapchain = VulkanViewport->GetSwapchain()->GetHandle();
		});
		ASSERT_TRUE(FirstSwapchain);

		GCommandListExecutor.ExecuteSynchronousOperation(false, []() {
			GDynamicRHI->RHIResetDiagnosticStatistics();
		});
		bool bRecordedAcquireWithoutPresent = false;
		ENQUEUE_RENDER_COMMAND(RecordAcquireWithoutPresent)(
			[Viewport, &bRecordedAcquireWithoutPresent](FRHICommandListImmediate& Commands) {
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				Commands.BeginDrawingViewport(Viewport, nullptr);
				FTextureRHIRef BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(Viewport);
				if (BackBuffer)
				{
					FRHIRenderPassInfo Pass;
					Pass.RenderTargetLayout.NumColorRenderTargets = 1;
					auto& Attachment = Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget;
					Attachment.Format = BackBuffer->GetFormat();
					Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
					Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
					Attachment.InitialLayout = ERHITextureLayout::Undefined;
					Attachment.InitialAccess = ERHIAccess::None;
					Attachment.FinalLayout = ERHITextureLayout::Present;
					Attachment.FinalAccess = ERHIAccess::Present;
					Pass.ColorRenderTargets[0] = BackBuffer;
					Pass.ColorClearValues[0] = FClearValueBinding(0.05f, 0.1f, 0.2f, 1.0f);
					Commands.BeginRenderPass(Pass, "AcquireWithoutPresent");
					Commands.EndRenderPass();
					bRecordedAcquireWithoutPresent = true;
				}
				Commands.EndDrawingViewport(Viewport, false, false);
			});
		FlushRenderingCommands();
		ASSERT_TRUE(bRecordedAcquireWithoutPresent);
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::SubmitToGPU);
		GDynamicRHI->RHIResizeViewport(Viewport, 68, 68, false);
		FlushRenderingCommands();
		FVulkanDebugMessageStatistics ResizeDiagnostics;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			ResizeDiagnostics = static_cast<FVulkanDynamicRHI*>(GDynamicRHI)
				->GetDebugMessageStatistics();
			ASSERT_NE(VulkanViewport->GetSwapchain(), nullptr);
			FirstSwapchain = VulkanViewport->GetSwapchain()->GetHandle();
		});
		EXPECT_EQ(ResizeDiagnostics.ValidationCount, 0u);

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain);
		GDynamicRHI->RHIResizeViewport(Viewport, 80, 80, false);
		FlushRenderingCommands();
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			ASSERT_NE(VulkanViewport->GetSwapchain(), nullptr);
			EXPECT_EQ(VulkanViewport->GetSwapchain()->GetHandle(), FirstSwapchain);
		});

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::SwapchainImageView);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
		});
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			EXPECT_EQ(VulkanViewport->GetSwapchain(), nullptr);
			EXPECT_FALSE(VulkanViewport->HasAvailableOutput());
		});

		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
		});
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			EXPECT_NE(VulkanViewport->GetSwapchain(), nullptr);
			EXPECT_TRUE(VulkanViewport->HasAvailableOutput());
		});
		auto RenderAndPresent = [](const TRefCountPtr<FRHIViewport>& InViewport,
			bool& bRecordedPresent) {
			ENQUEUE_RENDER_COMMAND(PublicRHIConformancePresent)(
				[InViewport, &bRecordedPresent](FRHICommandListImmediate& Commands) {
					Commands.SwitchPipeline(ERHIPipeline::Graphics);
					Commands.BeginDrawingViewport(InViewport, nullptr);
					FTextureRHIRef BackBuffer =
						GDynamicRHI->RHIGetViewportBackBuffer(InViewport);
					if (!BackBuffer)
					{
						Commands.EndDrawingViewport(InViewport, false, false);
						return;
					}
					FRHIRenderPassInfo Pass;
					Pass.RenderTargetLayout.NumColorRenderTargets = 1;
					auto& Attachment = Pass.RenderTargetLayout
						.ColorAttachments[0].RenderTarget;
					Attachment.Format = BackBuffer->GetFormat();
					Attachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
					Attachment.StoreAction = ERHIRenderTargetStoreAction::Store;
					Attachment.InitialLayout = ERHITextureLayout::Undefined;
					Attachment.InitialAccess = ERHIAccess::None;
					Attachment.FinalLayout = ERHITextureLayout::Present;
					Attachment.FinalAccess = ERHIAccess::Present;
					Pass.ColorRenderTargets[0] = BackBuffer;
					Pass.ColorClearValues[0] = FClearValueBinding(0.05f, 0.1f, 0.2f, 1);
					Commands.BeginRenderPass(Pass, "PublicRHIViewportPresent");
					Commands.EndRenderPass();
					Commands.EndDrawingViewport(InViewport, true, false);
					bRecordedPresent = true;
				});
			FlushRenderingCommands();
		};
		bool bMainPresentRecorded = false;
		RenderAndPresent(Viewport, bMainPresentRecorded);
		EXPECT_TRUE(bMainPresentRecorded);
		// The render-thread flush only records the present. Submit it before a
		// direct RHI-thread swapchain recreation can replace its image owners.
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::SubmitToGPU);
		for (uint32 Generation = 0; Generation < 5; ++Generation)
		{
			GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
				VulkanViewport->RecreateSwapchain();
				EXPECT_TRUE(VulkanViewport->HasAvailableOutput());
			});
		}
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->BeginDrawing();
			EXPECT_TRUE(VulkanViewport->HasAvailableOutput());
		});

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::SwapchainSemaphore);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
			EXPECT_FALSE(VulkanViewport->HasAvailableOutput());
			VulkanViewport->RecreateSwapchain();
			EXPECT_TRUE(VulkanViewport->HasAvailableOutput());
		});

		TRefCountPtr<FRHIViewport> DetachedViewport =
			GDynamicRHI->RHICreateViewport({
				.NativeWindowHandle =
					DetachedWindow->GetOSNativeWindowHandle(),
				.SizeX = 64,
				.SizeY = 64,
				.PreferredPixelFormat = EPixelFormat::SBGRA8_UNORM,
				.PresentationPolicy =
					EViewportPresentationPolicy::BestEffort});
		ASSERT_TRUE(DetachedViewport);
		auto* DetachedVulkanViewport = static_cast<FVulkanViewport*>(DetachedViewport.GetReference());
		GCommandListExecutor.ExecuteSynchronousOperation(false, [DetachedVulkanViewport]() {
			DetachedVulkanViewport->RecreateSwapchain();
			EXPECT_TRUE(DetachedVulkanViewport->HasAvailableOutput());
		});
		GDynamicRHI->RHIResizeViewport(DetachedViewport, 72, 72, false);
		FlushRenderingCommands();
		GCommandListExecutor.ExecuteSynchronousOperation(false, [DetachedVulkanViewport]() {
			EXPECT_TRUE(DetachedVulkanViewport->HasAvailableOutput());
		});
		vk::SwapchainKHR DetachedSwapchain = VK_NULL_HANDLE;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			ASSERT_NE(DetachedVulkanViewport->GetSwapchain(), nullptr);
			DetachedSwapchain = DetachedVulkanViewport->GetSwapchain()->GetHandle();
		});
		ArmVulkanSwapchainAcquireTimeoutForTest();
		bool bDetachedTimeoutSkippedFrame = false;
		ENQUEUE_RENDER_COMMAND(DetachedAcquireTimeoutSkipsFrame)(
			[DetachedViewport, &bDetachedTimeoutSkippedFrame](
				FRHICommandListImmediate& Commands) {
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
				Commands.BeginDrawingViewport(DetachedViewport, nullptr);
				bDetachedTimeoutSkippedFrame =
					!GDynamicRHI->RHIGetViewportBackBuffer(DetachedViewport);
				Commands.EndDrawingViewport(DetachedViewport, false, false);
			});
		FlushRenderingCommands();
		EXPECT_TRUE(bDetachedTimeoutSkippedFrame);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			ASSERT_NE(DetachedVulkanViewport->GetSwapchain(), nullptr);
			EXPECT_EQ(DetachedVulkanViewport->GetSwapchain()->GetHandle(),
				DetachedSwapchain);
			EXPECT_TRUE(DetachedVulkanViewport->HasAvailableOutput());
		});
		bool bDetachedPresentRecorded = false;
		RenderAndPresent(DetachedViewport, bDetachedPresentRecorded);
		EXPECT_TRUE(bDetachedPresentRecorded);
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThread,
			ERHISubmitFlags::SubmitToGPU);
		DetachedViewport = nullptr;

		Viewport = nullptr;
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
	}
} // namespace Durin::VulkanRHI
