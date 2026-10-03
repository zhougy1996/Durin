#pragma once

#include "CoreMinimal.h"

#include "RHI.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	struct FRHIRenderTargetsInfo;

	// Converts live RHI render targets into a Vulkan-compatible attachment layout.
	class FVulkanRenderTargetsLayout
	{
	public:
		FVulkanRenderTargetsLayout(FVulkanDevice& InDevice, const FRHIRenderTargetsInfo& InRenderTargetsInfo);
	};
}
