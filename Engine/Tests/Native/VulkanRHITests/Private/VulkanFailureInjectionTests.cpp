#include <gtest/gtest.h>

#include "Modules/ModuleManager.h"
#include "PCH.VulkanRHI.h"
#include "CoreGlobals.h"
#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RHIGlobals.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Shader/SlangShaderCompiler.h"
#include "VulkanRHIPrivate.h"
#include "VulkanDynamicRHI.h"
#include "VulkanExtensions.h"
#include "VulkanDevice.h"
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
			const auto Result = FModuleManager::Get().UnloadModule("VulkanRHI");
			EXPECT_TRUE(Result.Succeeded() || Result.Status == EModuleOperationStatus::NotLoaded ||
				Result.Status == EModuleOperationStatus::NotFound)
				<< Result.Message;
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
				_putenv_s("DURIN_RHI_EXECUTION", "threaded");
				ResetVulkanCreateFailures();
			}

			auto TearDown() -> void override
			{
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
			}

			std::optional<std::string> PreviousExecutionMode;
			std::optional<std::string> PreviousValidationMode;
		};
	}

	class FVulkanPublicRHIConformanceTests
		: public FVulkanCreateFailureInjectionTests
	{
	};

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
		EXPECT_FALSE(BuildVulkanInstanceExtensionRequest(Input).IsSuccess());

		Input.SurfaceProviderRequiredExtensions = {
			VK_KHR_SURFACE_EXTENSION_NAME,
			VK_KHR_SURFACE_EXTENSION_NAME,
			"VK_EXT_provider_surface"};
		Input.bRequirePortabilityEnumeration = true;
		const FVulkanInstanceExtensionRequest Result =
			BuildVulkanInstanceExtensionRequest(Input);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Diagnostic;
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
		EXPECT_NE(Result.Diagnostic.find("Vulkan 1.1"), std::string::npos);

		Input = MakeInstanceNegotiationInput();
		Input.AvailableExtensions.pop_back();
		Result = NegotiateVulkanInstance(Input);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Diagnostic.find(PlatformSurfaceExtension), std::string::npos);
		EXPECT_NE(Result.Diagnostic.find("platform required"), std::string::npos);
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
		ASSERT_TRUE(Result.IsSuccess()) << Result.Diagnostic;
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_SURFACE_EXTENSION_NAME)), 1);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)), 0);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)), 1);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)), 1);
	}

	TEST(FVulkanInstanceNegotiationTests, OptionalDiagnosticsActivateIndependently)
	{
		FVulkanInstanceNegotiationInput Input = MakeInstanceNegotiationInput();
		Input.bRequestDiagnostics = true;
		FVulkanInstanceNegotiationResult Result = NegotiateVulkanInstance(Input);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Diagnostic;
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
		EXPECT_TRUE(Result.IsSuccess()) << Result.Diagnostic;
		EXPECT_EQ(Result.EnabledExtensions.size(), 2u);
	}

	TEST(FVulkanDeviceCandidateTests, RejectsEveryHardRequirementBeforeRanking)
	{
		struct FCase
		{
			std::string_view ExpectedReason;
			std::function<void(FVulkanPhysicalDeviceCandidateInput&)> BreakRequirement;
		};
		const std::array Cases{
			FCase{"Vulkan 1.1", [](auto& Input) { Input.ApiVersion = VK_API_VERSION_1_0; }},
			FCase{"VK_KHR_swapchain", [](auto& Input) { Input.AvailableExtensions.clear(); }},
			FCase{"fillModeNonSolid", [](auto& Input) { Input.bFillModeNonSolid = false; }},
			FCase{"independentBlend", [](auto& Input) { Input.bIndependentBlend = false; }},
			FCase{"shaderDrawParameters", [](auto& Input) { Input.bShaderDrawParameters = false; }},
			FCase{"maxImageDimension2D", [](auto& Input) { Input.MaxImageDimension2D = 0; }},
			FCase{"maxImageDimensionCube", [](auto& Input) { Input.MaxImageDimensionCube = 0; }},
			FCase{"below six", [](auto& Input) { Input.MaxImageArrayLayers = 5; }},
			FCase{"maxComputeWorkGroupCount", [](auto& Input) {
				Input.MaxComputeWorkGroupCount[1] = 0;
			}},
			FCase{"graphics, compute, and presentation", [](auto& Input) {
				Input.QueueFamilies[0].bSupportsPresentation = false;
			}},
		};
		for (const FCase& Case : Cases)
		{
			FVulkanPhysicalDeviceCandidateInput Input = MakePhysicalDeviceCandidateInput();
			Case.BreakRequirement(Input);
			const FVulkanPhysicalDeviceCandidateEvaluation Result =
				EvaluateVulkanPhysicalDeviceCandidate(Input);
			ASSERT_FALSE(Result.IsSuitable());
			EXPECT_TRUE(std::ranges::any_of(Result.RejectionReasons,
				[&Case](const std::string& Reason) { return Reason.find(Case.ExpectedReason) != std::string::npos; }));
		}
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
		EXPECT_NE(Result.RejectionReasons.front().find(
			"VK_KHR_portability_subset"), std::string::npos);

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

	TEST(FVulkanDeviceCandidateTests, AllDeviceDiagnosticIsBoundedAndQualified)
	{
		std::vector<FVulkanPhysicalDeviceCandidateInput> Inputs(20);
		std::vector<FVulkanPhysicalDeviceCandidateEvaluation> Evaluations(20);
		for (size_t DeviceIndex = 0; DeviceIndex < Inputs.size(); ++DeviceIndex)
		{
			Inputs[DeviceIndex].DeviceName = std::format("GPU {}", DeviceIndex);
			for (size_t ReasonIndex = 0; ReasonIndex < 10; ++ReasonIndex)
				Evaluations[DeviceIndex].RejectionReasons.push_back(
					std::format("reason {} {}", ReasonIndex, std::string(300, 'x')));
		}
		const std::string Diagnostic =
			FormatVulkanPhysicalDeviceRejectionDiagnostic(Inputs, Evaluations);
		EXPECT_NE(Diagnostic.find("[0] GPU 0"), std::string::npos);
		EXPECT_NE(Diagnostic.find("[15] GPU 15"), std::string::npos);
		EXPECT_EQ(Diagnostic.find("GPU 16"), std::string::npos);
		EXPECT_NE(Diagnostic.find("(+4 devices)"), std::string::npos);
		EXPECT_NE(Diagnostic.find("(+2 reasons)"), std::string::npos);
		EXPECT_EQ(Diagnostic.find(std::string(257, 'x')), std::string::npos);
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
			std::pair{EVulkanCreateFailurePoint::Instance,
				std::string_view("Vulkan instance creation failed")},
			std::pair{EVulkanCreateFailurePoint::Device,
				std::string_view("Vulkan logical-device creation failed")},
			std::pair{EVulkanCreateFailurePoint::Allocator,
				std::string_view("Vulkan allocator creation failed")},
		};

		for (const auto& [FailurePoint, ExpectedDiagnostic] : FailureCases)
		{
			SCOPED_TRACE(ExpectedDiagnostic);
			ArmVulkanCreateFailure(FailurePoint);

			EXPECT_FALSE(RHIInit(GetVulkanTestInitializationContext()));
			EXPECT_EQ(GDynamicRHI, nullptr);
			EXPECT_FALSE(FModuleManager::Get().IsModuleLoaded("VulkanRHI"));
			EXPECT_NE(GetLastRHIInitializationDiagnostic().find(
				ExpectedDiagnostic), std::string_view::npos);
		}

		ASSERT_TRUE(RHIInit(GetVulkanTestInitializationContext()));
		EXPECT_TRUE(GetLastRHIInitializationDiagnostic().empty());
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
			"Begin:Stage2Outer", "Begin:Stage2Inner", "End", "End"}));

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
			Immediate.ImmediateFlush(EImmediateFlushType::FlushRHIThread,
				ERHISubmitFlags::SubmitToGPU);
			Orphaned = nullptr;
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
		Queries.reserve(512);
		for (uint32 Index = 0; Index < 512; ++Index)
		{
			FGPUTimingQueryRHIRef Query =
				GDynamicRHI->RHICreateGPUTimingQuery();
			ASSERT_TRUE(Query) << Index;
			Queries.push_back(std::move(Query));
		}
		EXPECT_FALSE(GDynamicRHI->RHICreateGPUTimingQuery());
		Statistics = GetVulkanGPUTimingStatisticsForTest(*VulkanRHI);
		EXPECT_EQ(Statistics.IntervalCapacity, 512u);
		EXPECT_EQ(Statistics.AllocatedPages, 8u);
		EXPECT_EQ(Statistics.LiveIntervals, 512u);
		EXPECT_EQ(Statistics.IntervalHighWater, 512u);
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
		EXPECT_EQ(Before.Timing.IntervalCapacity, 512u);
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
		EXPECT_EQ(After.GraphicsCache.GraphicsPipelines.Capacity,
			Before.GraphicsCache.GraphicsPipelines.Capacity);
		EXPECT_EQ(After.GraphicsCache.GraphicsPipelines.Occupancy,
			Before.GraphicsCache.GraphicsPipelines.Occupancy);
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
		EXPECT_EQ(Snapshots[0].GraphicsCache.GraphicsPipelines.Capacity,
			Snapshots[1].GraphicsCache.GraphicsPipelines.Capacity);
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
		ASSERT_TRUE(CompileOutput) << CompileOutput.ErrorMessage;
		ASSERT_EQ(CompileOutput.CompiledShaders.size(), 2u);

		std::array<std::vector<uint8>, 2> ModePixels;
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
				SampledBytes.data());
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
			FRHITextureCreateDesc Unsupported = FRHITextureCreateDesc::CreateCubeArray(
				"ExpectedUnsupportedConformanceTexture")
				.SetExtent(4).SetFormat(EPixelFormat::RGBA8_UNORM);
			EXPECT_FALSE(GDynamicRHI->RHIIsTextureSupported(Unsupported));
			EXPECT_FALSE(GDynamicRHI->RHICreateTexture(Commands, Unsupported));

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
			EXPECT_NEAR(ModePixels[ModeIndex][0], 64, 1);
			EXPECT_NEAR(ModePixels[ModeIndex][1], 128, 1);
			EXPECT_NEAR(ModePixels[ModeIndex][2], 191, 1);
			EXPECT_EQ(ModePixels[ModeIndex][3], 255);

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
			EXPECT_EQ(ModeSnapshots[ModeIndex].GraphicsCache
				.DescriptorSnapshots.Occupancy, 512u);
			EXPECT_EQ(ModeSnapshots[ModeIndex].GraphicsCache
				.DescriptorValueOccupancy, 1536u);
			EXPECT_GE(ModeSnapshots[ModeIndex].GraphicsCache
				.DescriptorSnapshots.Hits, 1u);
			EXPECT_GE(ModeSnapshots[ModeIndex].GraphicsCache
				.DescriptorSnapshots.Evictions, 1u);
			const FVulkanHotPathWorkTestStats HotPathWork =
				GetVulkanHotPathWorkTestStats();
			EXPECT_EQ(HotPathWork.BindingValidationVisits, 1542u);
			EXPECT_EQ(HotPathWork.DescriptorOccupancyVerificationVisits, 0u);
			EXPECT_EQ(HotPathWork.DescriptorOccupancyMutations, 514u);

			FRHIDiagnosticSnapshot StatisticsReset;
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				GDynamicRHI->RHIResetDiagnosticStatistics();
				StatisticsReset = GDynamicRHI->RHIGetDiagnosticSnapshot();
			});
			EXPECT_EQ(StatisticsReset.GraphicsCache.DescriptorSnapshots.Occupancy, 512u);
			EXPECT_EQ(StatisticsReset.GraphicsCache.DescriptorValueOccupancy, 1536u);
			EXPECT_EQ(StatisticsReset.GraphicsCache.DescriptorSnapshots.Hits, 0u);

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
		std::string ValidationError;
		ASSERT_TRUE(ValidateTextureCreateDesc(Oversized, ValidationError)) << ValidationError;
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
		ASSERT_TRUE(CompileOutput) << CompileOutput.ErrorMessage;
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
		const FRHIGraphicsCacheStatistics CacheStatsBefore =
			GDynamicRHI->RHIGetGraphicsCacheStatistics();
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
		const FRHIGraphicsCacheStatistics CacheStatsAfterCreation =
			GDynamicRHI->RHIGetGraphicsCacheStatistics();
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
		std::vector<uint8> MrtPixels;
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
		GDynamicRHI->RHIResetGraphicsCacheStatistics();
		const FRHIGraphicsCacheStatistics ResetCacheStats =
			GDynamicRHI->RHIGetGraphicsCacheStatistics();
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
		EXPECT_NE(GetLastRHIInitializationDiagnostic().find(
			"startup presentation surface"), std::string_view::npos);
		ExpectVulkanModuleUnloaded();

		ASSERT_TRUE(RHIInit(InitializationContext));
		RHIExit();
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
			.PresentModePolicy = EViewportPresentModePolicy::MainWindow,
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
				.PresentModePolicy =
					EViewportPresentModePolicy::ImGuiDetachedViewport});
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
		bool bDetachedPresentRecorded = false;
		RenderAndPresent(DetachedViewport, bDetachedPresentRecorded);
		EXPECT_TRUE(bDetachedPresentRecorded);
		DetachedViewport = nullptr;

		Viewport = nullptr;
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
	}
} // namespace Durin::VulkanRHI
