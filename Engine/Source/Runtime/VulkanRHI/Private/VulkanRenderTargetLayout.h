#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	struct FRHIRenderTargetsInfo;

	class FVulkanRenderTargetsLayout
	{
	public:
		FVulkanRenderTargetsLayout(FVulkanDevice& Device, const FRHIRenderTargetsInfo& RenderTargetsInfo);
	};
}