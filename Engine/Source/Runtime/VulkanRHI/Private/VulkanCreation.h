#pragma once

#include "CoreMinimal.h"
#include "Backend/RHICreation.h"

#include "RHI.h"

#include "RHICommandList.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	// Nullable factories log recoverable failures locally. Terminal exceptions propagate.
	template<typename Factory>
	auto CreateVulkanResource(Factory&& Create, std::string_view Kind,
		std::string_view DebugName = {}) -> std::invoke_result_t<Factory>
	{
		try
		{
			return std::forward<Factory>(Create)();
		}
		catch (const vk::SystemError& Error)
		{
			const auto Result = static_cast<vk::Result>(Error.code().value());
			if (!IsRecoverableVulkanCreationError(Result)) throw;
			DURIN_ERROR("Failed to create Vulkan {} '{}': {} (native code={})", Kind,
				DebugName.empty() ? "<unnamed>" : DebugName, Error.what(), Error.code().value());
			return {};
		}
		catch (const FRHIResourceCapacityError& Error)
		{
			DURIN_ERROR("Failed to create Vulkan {} '{}': {}", Kind,
				DebugName.empty() ? "<unnamed>" : DebugName, Error.what());
			return {};
		}
	}
}
