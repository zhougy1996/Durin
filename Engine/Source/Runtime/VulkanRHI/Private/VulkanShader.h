#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanDevice;

	class FVulkanShader
	{
	public:
		FVulkanShader(FVulkanDevice& Device, const std::string& Filename, vk::ShaderStageFlagBits Stage);

		~FVulkanShader();

		auto GetShaderModule() const -> vk::ShaderModule { return ShaderModule_; }

	protected:
		FVulkanDevice& Device_;

		vk::ShaderModule ShaderModule_;
	};
}