#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanDevice;

	class FVulkanShader : public FRHIShader
	{
	public:
		FVulkanShader(FVulkanDevice& InDevice, const FRHIShaderCreateDesc& InCreateDesc);

		~FVulkanShader() override;

		auto GetShaderModule() const -> vk::ShaderModule { return ShaderModule; }

		auto GetEntryPoint() const -> const char* { return EntryPoint; }

	protected:
		FVulkanDevice& Device;

		const char* EntryPoint = nullptr;

		vk::ShaderModule ShaderModule;
	};

}