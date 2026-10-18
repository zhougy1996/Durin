#include "CoreMinimal.h"
#include "VulkanExtensions.h"
#include "VulkanDevice.h"
#include <gtest/gtest.h>

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
		EXPECT_TRUE(Result.bEnableEvents);
		EXPECT_TRUE(Result.bEnableSwapchainMaintenance1);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)), 0);
	}

	TEST(FVulkanDeviceCandidateTests,
		PortabilitySubsetCanDisableEventsWithoutDisablingSynchronization2)
	{
		auto Input = MakePhysicalDeviceCandidateInput();
		Input.bRequirePortabilitySubset = true;
		Input.bEventsFeature = false;
		Input.bSynchronization2Feature = true;
		Input.AvailableExtensions.push_back("VK_KHR_portability_subset");
		const auto Result = EvaluateVulkanPhysicalDeviceCandidate(Input);
		ASSERT_TRUE(Result.IsSuitable());
		EXPECT_TRUE(Result.bEnableSynchronization2);
		EXPECT_FALSE(Result.bEnableEvents);
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
}
