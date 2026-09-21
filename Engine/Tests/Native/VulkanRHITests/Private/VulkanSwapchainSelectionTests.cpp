#include "PCH.VulkanRHI.h"
#include "VulkanSwapchain.h"

#include <gtest/gtest.h>

namespace Durin::VulkanRHI
{
	namespace
	{
		auto MakeInput() -> FVulkanSwapchainSelectionInput
		{
			FVulkanSwapchainSelectionInput Input;
			Input.Capabilities.minImageCount = 2;
			Input.Capabilities.maxImageCount = 4;
			Input.Capabilities.currentExtent = vk::Extent2D{UINT32_MAX, UINT32_MAX};
			Input.Capabilities.minImageExtent = vk::Extent2D{64, 64};
			Input.Capabilities.maxImageExtent = vk::Extent2D{1920, 1080};
			Input.Capabilities.currentTransform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
			Input.Capabilities.supportedUsageFlags = RequiredSwapchainImageUsage;
			Input.Capabilities.supportedCompositeAlpha =
				vk::CompositeAlphaFlagBitsKHR::eOpaque;
			Input.Formats = {{vk::Format::eB8G8R8A8Unorm,
				vk::ColorSpaceKHR::eSrgbNonlinear},
				{vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear}};
			Input.PresentModes = {vk::PresentModeKHR::eImmediate,
				vk::PresentModeKHR::eFifo};
			Input.RequestedWidth = 2560;
			Input.RequestedHeight = 32;
			return Input;
		}
	}

	TEST(FVulkanSwapchainSelectionTests, SelectsCompleteSupportedConfiguration)
	{
		auto Input = MakeInput();
		std::expected<FVulkanSwapchainConfiguration, std::string> Result;
		ASSERT_TRUE((Result = SelectVulkanSwapchainConfiguration(Input)))
			<< Result.error();
		EXPECT_EQ(Result->SurfaceFormat.format, vk::Format::eR8G8B8A8Srgb);
		EXPECT_EQ(Result->PresentMode, vk::PresentModeKHR::eFifo);
		EXPECT_EQ(Result->Extent, (vk::Extent2D{1920, 64}));
		EXPECT_EQ(Result->ImageCount, 2u);
		EXPECT_EQ(Result->ImageUsage, RequiredSwapchainImageUsage);
		EXPECT_EQ(Result->CompositeAlpha,
			vk::CompositeAlphaFlagBitsKHR::eOpaque);
	}

	TEST(FVulkanSwapchainSelectionTests, HonorsFixedExtentAndClampsImageCount)
	{
		auto Input = MakeInput();
		Input.Capabilities.currentExtent = vk::Extent2D{800, 600};
		Input.Capabilities.minImageCount = 3;
		Input.Capabilities.maxImageCount = 3;
		Input.PresentationPolicy = EViewportPresentationPolicy::BestEffort;
		Input.PresentModes = {vk::PresentModeKHR::eMailbox};
		std::expected<FVulkanSwapchainConfiguration, std::string> Result;
		ASSERT_TRUE((Result = SelectVulkanSwapchainConfiguration(Input)))
			<< Result.error();
		EXPECT_EQ(Result->Extent, (vk::Extent2D{800, 600}));
		EXPECT_EQ(Result->ImageCount, 3u);
		EXPECT_EQ(Result->PresentMode, vk::PresentModeKHR::eMailbox);
	}

	TEST(FVulkanSwapchainSelectionTests, BoundsOnlyBestEffortAcquisition)
	{
		EXPECT_EQ(GetSwapchainAcquireTimeout(
			EViewportPresentationPolicy::FramePaced), UINT64_MAX);
		EXPECT_EQ(GetSwapchainAcquireTimeout(
			EViewportPresentationPolicy::BestEffort),
			BestEffortAcquireTimeoutNanoseconds);
		EXPECT_GT(BestEffortAcquireTimeoutNanoseconds, 0u);
		EXPECT_LT(BestEffortAcquireTimeoutNanoseconds, UINT64_MAX);
	}

	TEST(FVulkanSwapchainSelectionTests, UsesDeterministicCompositeAlphaFallback)
	{
		const std::array Fallbacks{
			vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
			vk::CompositeAlphaFlagBitsKHR::ePostMultiplied,
			vk::CompositeAlphaFlagBitsKHR::eInherit};
		for (const auto Expected : Fallbacks)
		{
			auto Input = MakeInput();
			Input.Capabilities.supportedCompositeAlpha = Expected;
			std::expected<FVulkanSwapchainConfiguration, std::string> Result;
			ASSERT_TRUE((Result = SelectVulkanSwapchainConfiguration(Input)))
				<< Result.error();
			EXPECT_EQ(Result->CompositeAlpha, Expected);
		}
	}

	TEST(FVulkanSwapchainSelectionTests, UsesUndefinedFormatAndBestEffortFallbacks)
	{
		auto Input = MakeInput();
		Input.Formats = {{vk::Format::eUndefined,
			vk::ColorSpaceKHR::eSrgbNonlinear}};
		Input.PresentationPolicy = EViewportPresentationPolicy::BestEffort;
		Input.PresentModes = {vk::PresentModeKHR::eImmediate,
			vk::PresentModeKHR::eFifo};
		std::expected<FVulkanSwapchainConfiguration, std::string> Result;
		ASSERT_TRUE((Result = SelectVulkanSwapchainConfiguration(Input)))
			<< Result.error();
		EXPECT_EQ(Result->SurfaceFormat.format, vk::Format::eR8G8B8A8Srgb);
		EXPECT_EQ(Result->PresentMode, vk::PresentModeKHR::eImmediate);
	}

	TEST(FVulkanSwapchainSelectionTests, PrefersBgraSrgbOverLinearFallback)
	{
		auto Input = MakeInput();
		Input.Formats = {
			{vk::Format::eB8G8R8A8Unorm,
				vk::ColorSpaceKHR::eSrgbNonlinear},
			{vk::Format::eB8G8R8A8Srgb,
				vk::ColorSpaceKHR::eSrgbNonlinear}};
		std::expected<FVulkanSwapchainConfiguration, std::string> Result;
		ASSERT_TRUE((Result = SelectVulkanSwapchainConfiguration(Input))) << Result.error();
		EXPECT_EQ(Result->SurfaceFormat.format,
			vk::Format::eB8G8R8A8Srgb);
	}

	TEST(FVulkanSwapchainSelectionTests, RejectsIncompleteSurfaceSnapshots)
	{
		for (uint32 Case = 0; Case < 4; ++Case)
		{
			SCOPED_TRACE(Case);
			auto Input = MakeInput();
			if (Case == 0) Input.Formats.clear();
			if (Case == 1) Input.PresentModes.clear();
			if (Case == 2) Input.Capabilities.supportedUsageFlags =
				vk::ImageUsageFlagBits::eColorAttachment;
			if (Case == 3) Input.Capabilities.supportedCompositeAlpha = {};
			std::expected<FVulkanSwapchainConfiguration, std::string> Result;
			ASSERT_FALSE((Result = SelectVulkanSwapchainConfiguration(Input)));
			EXPECT_FALSE(Result.error().empty());
		}
	}

	TEST(FVulkanSwapchainSelectionTests, RejectsUnsupportedPolicyAndInvalidRanges)
	{
		for (uint32 Case = 0; Case < 3; ++Case)
		{
			SCOPED_TRACE(Case);
			auto Input = MakeInput();
			if (Case == 0) Input.PresentModes = {vk::PresentModeKHR::eImmediate};
			if (Case == 1)
			{
				Input.Capabilities.minImageCount = 3;
				Input.Capabilities.maxImageCount = 2;
			}
			if (Case == 2) Input.Capabilities.currentExtent = vk::Extent2D{0, 0};
			std::expected<FVulkanSwapchainConfiguration, std::string> Result;
			ASSERT_FALSE((Result = SelectVulkanSwapchainConfiguration(Input)));
			EXPECT_FALSE(Result.error().empty());
		}
	}
} // namespace Durin::VulkanRHI
