#pragma once

#include "VulkanRHIAPI.h"

namespace Durin::VulkanRHI
{
	// Classifies why a Vulkan requirement may be activated or rejected.
	enum class EVulkanRequirementClass : uint8
	{
		RequiredRuntime,
		PlatformRequired,
		OptionalFeature,
		OptionalDiagnostic,
		PromotedCore,
	};

	struct FVulkanRequirementState
	{
		std::string Name;
		EVulkanRequirementClass Class = EVulkanRequirementClass::OptionalFeature;
		bool bSupported = false;
		bool bRequested = false;
		bool bActivated = false;
	};

	struct FVulkanInstanceNegotiationInput
	{
		uint32 LoaderApiVersion = 0;
		std::vector<std::string> AvailableExtensions;
		std::vector<std::string> AvailableLayers;
		std::vector<std::string> PlatformRequiredExtensions;
		bool bRequestDiagnostics = false;
	};

	struct FVulkanInstanceExtensionRequestInput
	{
		std::vector<std::string> SurfaceProviderRequiredExtensions;
		bool bRequirePortabilityEnumeration = false;
	};

	struct FVulkanInstanceExtensionRequest
	{
		std::vector<std::string> RequiredExtensions;
		bool bEnablePortabilityEnumeration = false;
		std::string Diagnostic;

		auto IsSuccess() const -> bool { return Diagnostic.empty(); }
	};

	struct FVulkanInstanceNegotiationResult
	{
		uint32 ApiVersion = 0;
		std::vector<FVulkanRequirementState> Requirements;
		std::vector<std::string> EnabledExtensions;
		std::vector<std::string> EnabledLayers;
		std::string Diagnostic;

		auto IsSuccess() const -> bool { return Diagnostic.empty(); }
	};

	struct FVulkanValidationPolicy
	{
		bool bRequestDiagnostics = false;
		bool bInvalidSetting = false;
	};

	VULKANRHI_API auto ResolveVulkanValidationPolicy(
		const char* ConfiguredMode,
		bool bDebugBuild,
		bool bShippingBuild) -> FVulkanValidationPolicy;
	VULKANRHI_API auto BuildVulkanInstanceExtensionRequest(
		const FVulkanInstanceExtensionRequestInput& Input)
		-> FVulkanInstanceExtensionRequest;
	VULKANRHI_API auto NegotiateVulkanInstance(const FVulkanInstanceNegotiationInput& Input)
		-> FVulkanInstanceNegotiationResult;

}
