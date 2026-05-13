#pragma once

#include "VulkanCommon.h"
#include "VulkanDescriptorSets.h"
#include "VulkanShader.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanRenderPass;
	class FVulkanCommandListContext;
	class FVulkanCommandBuffer;
	class FVulkanShader;
	class FVulkanBuffer;
	class FVulkanTexture;

	class FVulkanGraphicsPipelineState : public FRHIGraphicsPipelineState
	{
	public:
		FVulkanGraphicsPipelineState(FVulkanDevice& InDevice, const FGraphicsPipelineStateInitializer& Initializer);

		~FVulkanGraphicsPipelineState() override;

		auto SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void;

		auto SetScissor(float MinX, float MinY, float Width, float Height) -> void;

		auto Bind(vk::CommandBuffer InCmdBuffer) -> void;

		auto PrepareForDraw(FVulkanCommandListContext& InContext) -> void;

		auto GetPipelineLayout() const -> vk::PipelineLayout { return PipelineLayout; }

		auto SetUniformBuffer(FVulkanCommandListContext& InContext, FRHIShader* InShader, uint32 SetIndex, uint32 BindIndex, FVulkanBuffer* InUniformBuffer) -> void;

		auto SetTexture(FVulkanCommandListContext& InContext, uint32 BindIndex, FVulkanTexture* InTexture) -> void;

		auto SetSampler(FVulkanCommandListContext& InContext, uint32 BindIndex, vk::Sampler InSampler) -> void;

	protected:
		const FVulkanRenderPass* RenderPass = nullptr;

		auto SetScissorRect(uint32 MinX, uint32 MinY, uint32 Width, uint32 Height) -> void;

		auto KeepShadersAlive() -> void;

		auto ReleaseShaders() -> void;

		FVulkanDevice& Device;

		vk::Viewport Viewport;

		vk::Rect2D Scissor;

		bool bScissorEnabled = false;

		FVulkanShader* Shaders[EShaderStage::Count] = { nullptr };

		vk::DescriptorSetLayout DescriptorSetLayout; // TODO: cache and share

		vk::PipelineLayout PipelineLayout;

		std::vector<vk::DescriptorSet> DescriptorSets; // TODO: cache and share

		vk::Pipeline Pipeline;

		friend class FVulkanGraphicsPipelineState;
		friend class FVulkanPipelineManager;
	};

	class FVulkanPipelineManager
	{
	public:
		FVulkanPipelineManager(FVulkanDevice& InDevice);
		~FVulkanPipelineManager();

		auto GetGraphicsPipelineState(FName Name) -> TRefCountPtr<FVulkanGraphicsPipelineState>;

		auto CreateGraphicsPipelineState(FName Name, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FVulkanGraphicsPipelineState>;

	private:
		FVulkanDevice& Device;

		std::unordered_map<FName, TRefCountPtr<FVulkanGraphicsPipelineState>> PSOCache;
	};

	class FPipelineCache
	{
	public:
		FVulkanDescriptorSetLayoutCache DSetLayoutMap;
	};
}