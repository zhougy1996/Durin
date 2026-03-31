#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanDevice;

	class FVulkanShader : public FRHIShader
	{
	public:
		FVulkanShader(FVulkanDevice& InDevice, const FRHIShaderCreateDesc& InCreateDesc);

		~FVulkanShader() override;

		auto GetShaderModule() const -> vk::ShaderModule { return ShaderModule; }

	protected:
		FVulkanDevice& Device;

		vk::ShaderModule ShaderModule;
	};

}