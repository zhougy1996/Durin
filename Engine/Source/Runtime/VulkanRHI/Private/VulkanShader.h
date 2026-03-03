#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanDevice;

	class FVulkanShader
	{
	public:
		FVulkanShader(FVulkanDevice& InDevice, const std::string& InFilename, vk::ShaderStageFlagBits InStage);

		~FVulkanShader();

		auto GetShaderModule() const -> vk::ShaderModule { return ShaderModule; }

	protected:
		FVulkanDevice& Device;

		vk::ShaderModule ShaderModule;
	};
}