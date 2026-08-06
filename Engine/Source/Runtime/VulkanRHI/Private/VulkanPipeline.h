#pragma once

#include "VulkanCommon.h"
#include "VulkanDescriptorSets.h"
#include "VulkanShader.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanRenderPass;
	class FVulkanCommandListContext;
	class FVulkanShader;
	class FVulkanBuffer;
	class FVulkanTexture;
	class FVulkanSampler;

	// Owns one complete Vulkan graphics pipeline and the layout used to bind it.
	class FVulkanGraphicsPipelineState : public FRHIGraphicsPipelineState
	{
	public:
		FVulkanGraphicsPipelineState(FVulkanDevice& InDevice, const FGraphicsPipelineStateInitializer& Initializer);

		~FVulkanGraphicsPipelineState() override;

		auto Bind(vk::CommandBuffer InCmdBuffer) -> void;

		auto GetPipelineLayout() const -> vk::PipelineLayout { return PipelineLayout; }

		auto GetDescriptorSetsLayout() const -> const FVulkanDescriptorSetsLayout&;
		auto PushConstants(FVulkanCommandListContext& InContext, EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* pValues) const -> void;

	protected:
		const FVulkanRenderPass* RenderPass = nullptr;

		FVulkanDevice& Device;

		FVulkanLayout* Layout = nullptr;

		vk::PipelineLayout PipelineLayout{};

		vk::Pipeline Pipeline{};

		friend class FVulkanPipelineManager;
	};

	// Creates graphics pipelines and owns reusable structural layout data.
	class FVulkanPipelineManager
	{
	public:
		FVulkanPipelineManager(FVulkanDevice& InDevice);
		~FVulkanPipelineManager();

		auto CreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FVulkanGraphicsPipelineState>;

		auto FindOrAddLayout(const FVulkanDescriptorSetsLayoutInfo& LayoutInfo) -> FVulkanLayout*;
	private:
		FVulkanDevice& Device;

		std::unordered_map<FVulkanDescriptorSetsLayoutInfo, FVulkanLayout*> LayoutMap;
	};

	// Owns the driver pipeline cache used to accelerate Vulkan pipeline creation.
	class FPipelineCache
	{
	public:
		FVulkanDescriptorSetLayoutCache DSetLayoutMap;
	};
}
