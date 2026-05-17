#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	struct FRHIRenderTargetsInfo;

	class FVulkanRenderTargetsLayout
	{
	public:
		FVulkanRenderTargetsLayout(FVulkanDevice& InDevice, const FRHIRenderTargetsInfo& InRenderTargetsInfo);
	};
}