#pragma once

#include "RHICommandList.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	// Caller-domain factory boundary. Diagnostics stay here; recovery callers may
	// optionally retain the typed failure. Terminal exceptions keep propagating.
	template<typename Factory>
	auto CreateVulkanResource(Factory&& Create, std::string_view Kind,
		std::string_view DebugName = {}, FRHICreationError* OutFailure = nullptr)
		-> std::invoke_result_t<Factory>
	{
		std::invoke_result_t<Factory> Resource;
		auto Error = ExecuteFallibleRHICreationOperation(MakeVulkanCreationOperation([&] {
			Resource = Create();
		}));
		if (!Resource && !Error.HasError())
			Error = {ERHIResourceCreationFailure::Unknown, ERHICreationFailureSource::BackendReturnedNull};
		if (OutFailure) *OutFailure = Error;
		if (Error.HasError())
		{
			DURIN_ERROR("Failed to create Vulkan {} '{}': {}", Kind,
				DebugName.empty() ? "<unnamed>" : DebugName, FormatRHICreationError(Error));
			return {};
		}
		return Resource;
	}
}
