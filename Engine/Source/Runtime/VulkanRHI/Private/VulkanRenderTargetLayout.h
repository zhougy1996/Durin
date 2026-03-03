#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	struct FRHIRenderTargetsInfo;

	class FVulkanRenderTargetsLayout
	{
	public:
		FVulkanRenderTargetsLayout(FVulkanDevice& InDevice, const FRHIRenderTargetsInfo& InRenderTargetsInfo);
	};
}