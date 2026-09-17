#pragma once

#include "VulkanRHIAPI.h"

namespace Durin::VulkanRHI
{
	enum class EVulkanError : uint8
	{
		None,
		LoaderVersionTooOld,
		MissingInstanceExtension,
		DeviceVersionTooOld,
		MissingSwapchainExtension,
		MissingPortabilitySubset,
		MissingFillModeNonSolid,
		MissingIndependentBlend,
		MissingShaderDrawParameters,
		InvalidImageDimension2D,
		InvalidImageDimensionCube,
		InsufficientArrayLayers,
		InvalidComputeWorkGroupCount,
		MissingPresentationQueue,
		MissingGraphicsComputeQueue,
		NoSurfaceFormats,
		NoPresentModes,
		UnsupportedImageUsage,
		InvalidExtentRange,
		InvalidImageCountRange,
		UnsupportedPresentPolicy,
		EmptyExtent,
		InsufficientImageCount,
		UnsupportedCompositeAlpha,
	};
	struct FVulkanError
	{
		EVulkanError Code = EVulkanError::None;
		// Identifiers and numeric facts, not diagnostic prose.
		std::string Requirement;
		uint32 Actual = 0;
		uint32 Required = 0;
	};
	struct FVulkanOperationResult
	{
		FVulkanError Error;
		auto IsSuccess() const -> bool { return Error.Code == EVulkanError::None; }
		explicit operator bool() const { return IsSuccess(); }
	};
	VULKANRHI_API auto FormatVulkanError(const FVulkanError& Error) -> std::string;
	VULKANRHI_API auto FormatVulkanErrors(std::span<const FVulkanError> Errors) -> std::string;
}
