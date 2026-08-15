#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanDevice;

	// Owns a Vulkan shader module for one compiled RHI shader stage.
	class FVulkanShader : public FRHIShader
	{
	public:
		FVulkanShader(FVulkanDevice& InDevice, const FRHIShaderCreateDesc& InCreateDesc);

		~FVulkanShader() override;

		auto GetShaderModule() const -> vk::ShaderModule { return ShaderModule; }

		auto GetEntryPoint() const -> const char* { return EntryPoint; }
		auto HasReflectedVertexInputs() const -> bool
		{
			return bHasReflectedVertexInputs;
		}
		auto ConsumesVertexAttribute(uint32 Location) const -> bool
		{
			return VertexInputLocations.contains(Location);
		}

	protected:
		FVulkanDevice& Device;

		const char* EntryPoint = nullptr;
		bool bHasReflectedVertexInputs = false;
		std::unordered_set<uint32> VertexInputLocations;

		vk::ShaderModule ShaderModule{};
	};

}
