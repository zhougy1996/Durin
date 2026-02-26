#pragma once

#include "RHIPipeline.h"
#include "VulkanCommon.h"

class FGraphicsPipelineStateInitializer;

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
		FVulkanGraphicsPipelineState(FVulkanDevice& Device, const FGraphicsPipelineStateInitializer& Initializer);

		auto SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void;

		auto SetScissor(float MinX, float MinY, float Width, float Height) -> void;

		auto Bind(vk::CommandBuffer CmdBuffer) -> void;

	private:
		const FVulkanRenderPass* RenderPass_ = nullptr;

		auto SetScissorRect(uint32 MinX, uint32 MinY, uint32 Width, uint32 Height) -> void;

		FVulkanDevice& Device_;

		vk::Viewport Viewport_;

		vk::Rect2D Scissor_;

		bool bScissorEnabled_ = false;

		FVulkanShader* Shaders_[NUM_SHADER_STAGES];

		vk::Pipeline Pipeline_;

		friend class FVulkanGraphicsPipelineState;
		friend class FVulkanPipelineManager;
	};

	class FVulkanPipelineManager
	{
	public:
		FVulkanPipelineManager(FVulkanDevice& Device);

		auto CreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer) -> TSharedPtr<FVulkanGraphicsPipelineState>;

	private:
		FVulkanDevice& Device_;
	};
}