#include "VulkanExtensions.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto ContainsName(const std::vector<std::string>& Names, std::string_view Name) -> bool
		{
			return std::ranges::any_of(Names,
				[Name](const std::string& Candidate) { return Candidate == Name; });
		}

		auto AddUnique(std::vector<std::string>& Names, std::string_view Name) -> void
		{
			if (!ContainsName(Names, Name)) Names.emplace_back(Name);
		}

		auto RequirementClassName(EVulkanRequirementClass Class) -> std::string_view
		{
			switch (Class)
			{
			case EVulkanRequirementClass::RequiredRuntime: return "required runtime";
			case EVulkanRequirementClass::PlatformRequired: return "platform required";
			case EVulkanRequirementClass::OptionalFeature: return "optional feature";
			case EVulkanRequirementClass::OptionalDiagnostic: return "optional diagnostic";
			case EVulkanRequirementClass::PromotedCore: return "promoted core";
			default: return "unknown";
			}
		}

	}

	auto ResolveVulkanValidationPolicy(
		const char* ConfiguredMode,
		bool bDebugBuild,
		bool bShippingBuild) -> FVulkanValidationPolicy
	{
		FVulkanValidationPolicy Result;
		if (bShippingBuild) return Result;
		const std::string_view Mode = ConfiguredMode ? ConfiguredMode : "auto";
		if (Mode == "off") return Result;
		if (Mode == "on")
		{
			Result.bRequestDiagnostics = true;
			return Result;
		}
		if (Mode != "auto") Result.bInvalidSetting = true;
		Result.bRequestDiagnostics = bDebugBuild;
		return Result;
	}

	auto BuildVulkanInstanceExtensionRequest(
		const FVulkanInstanceExtensionRequestInput& Input)
		-> FVulkanInstanceExtensionRequest
	{
		FVulkanInstanceExtensionRequest Result;
		for (const std::string& Extension : Input.SurfaceProviderRequiredExtensions)
		{
			if (!Extension.empty()) AddUnique(Result.RequiredExtensions, Extension);
		}
		if (Result.RequiredExtensions.empty())
		{
			Result.Diagnostic =
				"Vulkan surface provider reported no required instance extensions.";
			return Result;
		}
		Result.bEnablePortabilityEnumeration = Input.bRequirePortabilityEnumeration;
		if (Input.bRequirePortabilityEnumeration)
			AddUnique(Result.RequiredExtensions,
				VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		return Result;
	}

	auto NegotiateVulkanInstance(const FVulkanInstanceNegotiationInput& Input)
		-> FVulkanInstanceNegotiationResult
	{
		FVulkanInstanceNegotiationResult Result;
		constexpr uint32 MinimumApiVersion = VK_API_VERSION_1_1;
		constexpr uint32 MaximumApiVersion = VK_API_VERSION_1_3;
		Result.ApiVersion = std::min(Input.LoaderApiVersion, MaximumApiVersion);
		if (Input.LoaderApiVersion < MinimumApiVersion)
		{
			Result.Diagnostic = std::format(
				"Vulkan loader API {}.{}.{} is below required runtime Vulkan 1.1.",
				vk::apiVersionMajor(Input.LoaderApiVersion),
				vk::apiVersionMinor(Input.LoaderApiVersion),
				vk::apiVersionPatch(Input.LoaderApiVersion));
			return Result;
		}

		auto AddRequirement = [&](std::string_view Name, EVulkanRequirementClass Class,
			bool bRequested, bool bActivateWhenSupported) -> FVulkanRequirementState& {
			const auto Existing = std::ranges::find_if(Result.Requirements,
				[Name](const FVulkanRequirementState& Requirement) { return Requirement.Name == Name; });
			if (Existing != Result.Requirements.end())
			{
				if (Class == EVulkanRequirementClass::PlatformRequired)
					Existing->Class = Class;
				Existing->bRequested |= bRequested;
				Existing->bActivated |= bActivateWhenSupported && Existing->bSupported;
				return *Existing;
			}
			FVulkanRequirementState& Requirement = Result.Requirements.emplace_back();
			Requirement.Name = Name;
			Requirement.Class = Class;
			Requirement.bSupported = ContainsName(Input.AvailableExtensions, Name);
			Requirement.bRequested = bRequested;
			Requirement.bActivated = bActivateWhenSupported && Requirement.bSupported;
			return Requirement;
		};

		for (const std::string& Name : Input.PlatformRequiredExtensions)
		{
			FVulkanRequirementState& Requirement = AddRequirement(
				Name, EVulkanRequirementClass::PlatformRequired, true, true);
			if (!Requirement.bSupported)
			{
				if (!Result.Diagnostic.empty()) Result.Diagnostic += " ";
				Result.Diagnostic += std::format("Missing {} Vulkan instance extension '{}'.",
					RequirementClassName(Requirement.Class), Requirement.Name);
			}
		}
		if (!Result.Diagnostic.empty()) return Result;

		const bool bHasSurfaceCapabilities2 = ContainsName(
			Input.AvailableExtensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
		const bool bHasSurfaceMaintenance = ContainsName(
			Input.AvailableExtensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
		const bool bEnableSurfaceMaintenance = bHasSurfaceCapabilities2 && bHasSurfaceMaintenance;
		AddRequirement(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
			EVulkanRequirementClass::OptionalFeature, true, bEnableSurfaceMaintenance);
		AddRequirement(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
			EVulkanRequirementClass::OptionalFeature, true, bEnableSurfaceMaintenance);
		AddRequirement(VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
			EVulkanRequirementClass::OptionalDiagnostic,
			Input.bRequestDiagnostics, Input.bRequestDiagnostics);

		FVulkanRequirementState& Promoted = Result.Requirements.emplace_back();
		Promoted.Name = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
		Promoted.Class = EVulkanRequirementClass::PromotedCore;
		Promoted.bSupported = true;
		Promoted.bRequested = true;
		Promoted.bActivated = true;

		for (const FVulkanRequirementState& Requirement : Result.Requirements)
		{
			if (Requirement.bActivated && Requirement.Class != EVulkanRequirementClass::PromotedCore)
				AddUnique(Result.EnabledExtensions, Requirement.Name);
		}

		FVulkanRequirementState& ValidationLayer = Result.Requirements.emplace_back();
		ValidationLayer.Name = "VK_LAYER_KHRONOS_validation";
		ValidationLayer.Class = EVulkanRequirementClass::OptionalDiagnostic;
		ValidationLayer.bSupported = ContainsName(Input.AvailableLayers, ValidationLayer.Name);
		ValidationLayer.bRequested = Input.bRequestDiagnostics;
		ValidationLayer.bActivated = ValidationLayer.bRequested && ValidationLayer.bSupported;
		if (ValidationLayer.bActivated) Result.EnabledLayers.push_back(ValidationLayer.Name);
		return Result;
	}

}
