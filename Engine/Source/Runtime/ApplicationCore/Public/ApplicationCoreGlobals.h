#pragma once

#include "ApplicationCoreAPI.h"

namespace Durin
{
	class FGenericApplication;

	// Owns one Vulkan surface-requirements snapshot and its acquisition diagnostic.
	struct FVulkanSurfaceRequirementsResult
	{
		std::vector<std::string> RequiredInstanceExtensions;
		std::string Diagnostic;

		auto Succeeded() const -> bool { return Diagnostic.empty(); }
	};

	extern APPLICATIONCORE_API std::shared_ptr<FGenericApplication> GApp;

	APPLICATIONCORE_API auto InitializeApplicationCore() -> bool;

	APPLICATIONCORE_API auto IsApplicationCoreInitialized() -> bool;

	// Returns an owned snapshot while ApplicationCore holds an active lifecycle lease.
	APPLICATIONCORE_API auto GetVulkanSurfaceRequirements()
		-> FVulkanSurfaceRequirementsResult;

	APPLICATIONCORE_API auto ShutdownApplicationCore() -> void;
}
