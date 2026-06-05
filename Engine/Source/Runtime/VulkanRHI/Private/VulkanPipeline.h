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

		auto KeepShadersAlive() -> void;

		auto ReleaseShaders() -> void;

		FVulkanDevice& Device;

		FVulkanShader* Shaders[EShaderStage::Count] = { nullptr };

		FVulkanLayout* Layout;

		vk::PipelineLayout PipelineLayout;

		vk::Pipeline Pipeline;

		friend class FVulkanGraphicsPipelineState;
		friend class FVulkanPipelineStateCacheManager;
	};

	class FVulkanPipelineStateCacheManager
	{
	public:
		FVulkanPipelineStateCacheManager(FVulkanDevice& InDevice);
		~FVulkanPipelineStateCacheManager();

		auto GetGraphicsPipelineState(FName Name) -> TRefCountPtr<FVulkanGraphicsPipelineState>;

		auto CreateGraphicsPipelineState(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FVulkanGraphicsPipelineState>;

		auto FindOrAddLayout(const FVulkanDescriptorSetsLayoutInfo& LayoutInfo) -> FVulkanLayout*;
	private:
		FVulkanDevice& Device;

		std::unordered_map<FVulkanDescriptorSetsLayoutInfo, FVulkanLayout*> LayoutMap;

		std::unordered_map<FName, TRefCountPtr<FVulkanGraphicsPipelineState>> PSOCache;
	};

	class FPipelineCache
	{
	public:
		FVulkanDescriptorSetLayoutCache DSetLayoutMap;
	};
}
