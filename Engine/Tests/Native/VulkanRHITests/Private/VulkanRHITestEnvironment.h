#pragma once

#include "RHIInitialization.h"

namespace Durin::VulkanRHI
{
	// Supplies the explicit initialization mode required by Vulkan integration tests.
	auto GetVulkanTestInitializationContext() -> FRHIInitializationContext;
}
