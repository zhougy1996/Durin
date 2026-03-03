#pragma once

#include "RHIPipeline.h"
#include "VulkanCommon.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanRenderPass;
	class FVulkanCommandListContext;
	class FVulkanCommandBuffer;
	class FVulkanShader;

	class FVulkanGraphicsPipelineState : public FRHIGraphicsPipelineState
	{
	public:
		FVulkanGraphicsPipelineState(FVulkanDevice& InDevice, const FGraphicsPipelineStateInitializer& Initializer);

		auto SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void;

		auto SetScissor(float MinX, float MinY, float Width, float Height) -> void;

		auto Bind(vk::CommandBuffer InCmdBuffer) -> void;

		auto PrepareForDraw(FVulkanCommandListContext& InContext) -> void;

	private:
		const FVulkanRenderPass* RenderPass = nullptr;

		auto SetScissorRect(uint32 MinX, uint32 MinY, uint32 Width, uint32 Height) -> void;

		FVulkanDevice& Device;

		vk::Viewport Viewport;

		vk::Rect2D Scissor;

		bool bScissorEnabled = false;

		FVulkanShader* Shaders[NUM_SHADER_STAGES];

		vk::Pipeline Pipeline;

		friend class FVulkanGraphicsPipelineState;
		friend class FVulkanPipelineManager;
	};

	class FVulkanPipelineManager
	{
	public:
		FVulkanPipelineManager(FVulkanDevice& InDevice);

		auto CreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer) -> TSharedPtr<FVulkanGraphicsPipelineState>;

	private:
		FVulkanDevice& Device;
	};
}