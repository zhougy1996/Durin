#pragma once

#include "RHIShaderParameters.h"

namespace Durin::VulkanRHI
{
	class FVulkanCommandListContext;
	class FVulkanDevice;
	class FVulkanGraphicsPipelineState;

	class FVulkanPendingGraphicsState
	{
	public:
		explicit FVulkanPendingGraphicsState(FVulkanDevice& InDevice);

		auto SetGraphicsPipelineState(FVulkanGraphicsPipelineState& InPipelineState, vk::CommandBuffer InCmdBuffer) -> void;

		auto SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void;

		auto SetScissor(float MinX, float MinY, float Width, float Height) -> void;

		auto SetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void;

		auto PrepareForDraw(FVulkanCommandListContext& InContext) -> void;

		auto ClearDescriptorSetCache() -> void;

		auto Reset() -> void;

		auto GetPipelineState() const -> FVulkanGraphicsPipelineState* { return PipelineState; }

	private:
		struct FVulkanDescriptorSetCacheEntry
		{
			uint64 Hash = 0;
			uint64 LayoutHash = 0;
			std::vector<FRHIShaderParameterResource> Resources;
			std::vector<vk::DescriptorSet> DescriptorSets;
		};

		auto SetScissorRect(uint32 MinX, uint32 MinY, uint32 Width, uint32 Height) -> void;

		auto CalculatePendingDescriptorHash(uint64 LayoutHash) const -> uint64;

		static auto AreDescriptorResourcesEqual(
			const std::vector<FRHIShaderParameterResource>& A,
			const std::vector<FRHIShaderParameterResource>& B
		) -> bool;

		auto GetOrCreateDescriptorSetsForDraw() -> const std::vector<vk::DescriptorSet>&;

		FVulkanDevice& Device;

		FVulkanGraphicsPipelineState* PipelineState = nullptr;

		vk::Viewport Viewport;

		vk::Rect2D Scissor;

		std::vector<FRHIShaderParameterResource> PendingShaderResources;

		std::vector<FVulkanDescriptorSetCacheEntry> DescriptorSetCache;
	};
} // namespace Durin::VulkanRHI
