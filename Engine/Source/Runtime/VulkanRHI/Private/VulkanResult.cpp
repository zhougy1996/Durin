#include "VulkanResult.h"

namespace Durin::VulkanRHI
{
	auto FormatVulkanError(const FVulkanError& Error) -> std::string
	{
		std::string Text;
		switch (Error.Code)
		{
		case EVulkanError::None: return {};
		case EVulkanError::LoaderVersionTooOld: Text = "Vulkan loader API is below required runtime Vulkan 1.1."; break;
		case EVulkanError::MissingInstanceExtension: Text = "Missing platform required Vulkan instance extension."; break;
		case EVulkanError::DeviceVersionTooOld: Text = "device API version is below Vulkan 1.1"; break;
		case EVulkanError::MissingSwapchainExtension: Text = "missing platform required extension VK_KHR_swapchain"; break;
		case EVulkanError::MissingPortabilitySubset: Text = "missing platform required extension VK_KHR_portability_subset"; break;
		case EVulkanError::MissingFillModeNonSolid: Text = "missing required fillModeNonSolid feature"; break;
		case EVulkanError::MissingIndependentBlend: Text = "missing required independentBlend feature"; break;
		case EVulkanError::MissingShaderDrawParameters: Text = "missing required shaderDrawParameters feature"; break;
		case EVulkanError::InvalidImageDimension2D: Text = "maxImageDimension2D is zero"; break;
		case EVulkanError::InvalidImageDimensionCube: Text = "maxImageDimensionCube is zero"; break;
		case EVulkanError::InsufficientArrayLayers: Text = "maxImageArrayLayers is below six"; break;
		case EVulkanError::InvalidComputeWorkGroupCount: Text = "maxComputeWorkGroupCount contains a zero limit"; break;
		case EVulkanError::MissingPresentationQueue: Text = "no queue family provides graphics, compute, and presentation for the startup surface"; break;
		case EVulkanError::MissingGraphicsComputeQueue: Text = "no queue family provides graphics and compute"; break;
		case EVulkanError::NoSurfaceFormats: Text = "The surface reported no formats."; break;
		case EVulkanError::NoPresentModes: Text = "The surface reported no present modes."; break;
		case EVulkanError::UnsupportedImageUsage: Text = "Required backbuffer image usage is unsupported."; break;
		case EVulkanError::InvalidExtentRange: Text = "The surface extent range is invalid."; break;
		case EVulkanError::InvalidImageCountRange: Text = "The surface image-count range is invalid."; break;
		case EVulkanError::UnsupportedPresentPolicy: Text = "No policy-compatible present mode is supported."; break;
		case EVulkanError::EmptyExtent: Text = "The selected extent is empty."; break;
		case EVulkanError::InsufficientImageCount: Text = "The supported image count is below the frames-in-flight requirement."; break;
		case EVulkanError::UnsupportedCompositeAlpha: Text = "The surface reported no supported composite-alpha mode."; break;
		}
		if (!Error.Requirement.empty()) Text += std::format(" '{}'", Error.Requirement);
		if (Error.Required) Text += std::format(" actual={} required={}", Error.Actual, Error.Required);
		return Text;
	}
	auto FormatVulkanErrors(std::span<const FVulkanError> Errors) -> std::string
	{
		std::string Text;
		for (const auto& Error : Errors)
		{
			if (!Text.empty()) Text += " ";
			Text += FormatVulkanError(Error);
		}
		return Text;
	}
}
