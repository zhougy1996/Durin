#pragma once

#include <expected>

#include "RHICommandList.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	// Translate recoverable backend exceptions without logging. Terminal exceptions propagate.
	template<typename Factory>
	auto TryCreateVulkanResource(Factory&& Create)
		-> std::expected<std::invoke_result_t<Factory>, FRHICreationError>
	{
		std::invoke_result_t<Factory> Resource;
		const auto Error = ExecuteFallibleRHICreationOperation(MakeVulkanCreationOperation([&] {
			Resource = Create();
		}));
		if (Error.HasError()) return std::unexpected(Error);
		if (!Resource)
			return std::unexpected(FRHICreationError{ERHIResourceCreationFailure::Unknown,
				ERHICreationFailureSource::BackendReturnedNull});
		return Resource;
	}

	// Nullable factories consume recoverable failures and own their diagnostic.
	template<typename Factory>
	auto CreateVulkanResource(Factory&& Create, std::string_view Kind,
		std::string_view DebugName = {}) -> std::invoke_result_t<Factory>
	{
		auto Result = TryCreateVulkanResource(std::forward<Factory>(Create));
		if (!Result)
		{
			DURIN_ERROR("Failed to create Vulkan {} '{}': {}", Kind,
				DebugName.empty() ? "<unnamed>" : DebugName, FormatRHICreationError(Result.error()));
			return {};
		}
		return std::move(*Result);
	}
}
