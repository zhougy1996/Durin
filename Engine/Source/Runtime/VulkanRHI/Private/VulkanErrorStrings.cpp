#include "VulkanExtensions.h"
#include "VulkanSwapchain.h"

namespace Durin::VulkanRHI
{
	auto ToString(EVulkanInstanceNegotiationError Error) -> std::string_view
	{
		switch (Error)
		{
		case EVulkanInstanceNegotiationError::LoaderVersionTooOld: return "Vulkan loader API is below required runtime Vulkan 1.1.";
		case EVulkanInstanceNegotiationError::MissingInstanceExtension: return "Missing platform required Vulkan instance extension.";
		}
		return {};
	}

	auto ToString(const FVulkanInstanceNegotiationError& Error) -> std::string
	{
		std::string Text(ToString(Error.Code));
		if (!Error.Requirement.empty()) Text += std::format(" '{}'", Error.Requirement);
		if (Error.Required) Text += std::format(" actual={} required={}", Error.Actual, Error.Required);
		return Text;
	}

	auto ToString(EVulkanSwapchainSelectionError Error) -> std::string_view
	{
		switch (Error)
		{
		case EVulkanSwapchainSelectionError::NoSurfaceFormats: return "The surface reported no formats.";
		case EVulkanSwapchainSelectionError::NoPresentModes: return "The surface reported no present modes.";
		case EVulkanSwapchainSelectionError::UnsupportedImageUsage: return "Required backbuffer image usage is unsupported.";
		case EVulkanSwapchainSelectionError::InvalidExtentRange: return "The surface extent range is invalid.";
		case EVulkanSwapchainSelectionError::InvalidImageCountRange: return "The surface image-count range is invalid.";
		case EVulkanSwapchainSelectionError::UnsupportedPresentPolicy: return "No policy-compatible present mode is supported.";
		case EVulkanSwapchainSelectionError::EmptyExtent: return "The selected extent is empty.";
		case EVulkanSwapchainSelectionError::InsufficientImageCount: return "The supported image count is below the frames-in-flight requirement.";
		case EVulkanSwapchainSelectionError::UnsupportedCompositeAlpha: return "The surface reported no supported composite-alpha mode.";
		}
		return {};
	}

	auto FormatVulkanInstanceNegotiationErrors(std::span<const FVulkanInstanceNegotiationError> Errors) -> std::string
	{
		std::string Text;
		for (const auto& Error : Errors)
		{
			if (!Text.empty()) Text += " ";
			Text += ToString(Error);
		}
		return Text;
	}
}
