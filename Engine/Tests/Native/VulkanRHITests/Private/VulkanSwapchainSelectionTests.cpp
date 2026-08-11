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
		FVulkanSwapchainConfiguration Configuration;
		std::string Error;
		ASSERT_TRUE(SelectVulkanSwapchainConfiguration(Input, Configuration, Error))
			<< Error;
		EXPECT_EQ(Configuration.SurfaceFormat.format, vk::Format::eR8G8B8A8Srgb);
		EXPECT_EQ(Configuration.PresentMode, vk::PresentModeKHR::eFifo);
		EXPECT_EQ(Configuration.Extent, (vk::Extent2D{1920, 64}));
		EXPECT_EQ(Configuration.ImageCount, 2u);
		EXPECT_EQ(Configuration.ImageUsage, RequiredSwapchainImageUsage);
		EXPECT_EQ(Configuration.CompositeAlpha,
			vk::CompositeAlphaFlagBitsKHR::eOpaque);
	}

	TEST(FVulkanSwapchainSelectionTests, HonorsFixedExtentAndClampsImageCount)
	{
		auto Input = MakeInput();
		Input.Capabilities.currentExtent = vk::Extent2D{800, 600};
		Input.Capabilities.minImageCount = 3;
		Input.Capabilities.maxImageCount = 3;
		Input.PresentModePolicy = EViewportPresentModePolicy::ImGuiDetachedViewport;
		Input.PresentModes = {vk::PresentModeKHR::eMailbox};
		FVulkanSwapchainConfiguration Configuration;
		std::string Error;
		ASSERT_TRUE(SelectVulkanSwapchainConfiguration(Input, Configuration, Error))
			<< Error;
		EXPECT_EQ(Configuration.Extent, (vk::Extent2D{800, 600}));
		EXPECT_EQ(Configuration.ImageCount, 3u);
		EXPECT_EQ(Configuration.PresentMode, vk::PresentModeKHR::eMailbox);
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
			FVulkanSwapchainConfiguration Configuration;
			std::string Error;
			ASSERT_TRUE(SelectVulkanSwapchainConfiguration(Input, Configuration, Error))
				<< Error;
			EXPECT_EQ(Configuration.CompositeAlpha, Expected);
		}
	}

	TEST(FVulkanSwapchainSelectionTests, UsesUndefinedFormatAndDetachedModeFallbacks)
	{
		auto Input = MakeInput();
		Input.Formats = {{vk::Format::eUndefined,
			vk::ColorSpaceKHR::eSrgbNonlinear}};
		Input.PresentModePolicy = EViewportPresentModePolicy::ImGuiDetachedViewport;
		Input.PresentModes = {vk::PresentModeKHR::eImmediate,
			vk::PresentModeKHR::eFifo};
		FVulkanSwapchainConfiguration Configuration;
		std::string Error;
		ASSERT_TRUE(SelectVulkanSwapchainConfiguration(Input, Configuration, Error))
			<< Error;
		EXPECT_EQ(Configuration.SurfaceFormat.format, vk::Format::eR8G8B8A8Srgb);
		EXPECT_EQ(Configuration.PresentMode, vk::PresentModeKHR::eImmediate);
	}

	TEST(FVulkanSwapchainSelectionTests, RejectsIncompleteSurfaceSnapshots)
	{
		for (uint32 Case = 0; Case < 4; ++Case)
		{
			auto Input = MakeInput();
			if (Case == 0) Input.Formats.clear();
			if (Case == 1) Input.PresentModes.clear();
			if (Case == 2) Input.Capabilities.supportedUsageFlags =
				vk::ImageUsageFlagBits::eColorAttachment;
			if (Case == 3) Input.Capabilities.supportedCompositeAlpha = {};
			FVulkanSwapchainConfiguration Configuration;
			std::string Error;
			EXPECT_FALSE(SelectVulkanSwapchainConfiguration(Input, Configuration, Error));
			EXPECT_FALSE(Error.empty());
		}
	}

	TEST(FVulkanSwapchainSelectionTests, RejectsUnsupportedPolicyAndInvalidRanges)
	{
		for (uint32 Case = 0; Case < 3; ++Case)
		{
			auto Input = MakeInput();
			if (Case == 0) Input.PresentModes = {vk::PresentModeKHR::eImmediate};
			if (Case == 1)
			{
				Input.Capabilities.minImageCount = 3;
				Input.Capabilities.maxImageCount = 2;
			}
			if (Case == 2) Input.Capabilities.currentExtent = vk::Extent2D{0, 0};
			FVulkanSwapchainConfiguration Configuration;
			std::string Error;
			EXPECT_FALSE(SelectVulkanSwapchainConfiguration(Input, Configuration, Error));
			EXPECT_FALSE(Error.empty());
		}
	}
} // namespace Durin::VulkanRHI
