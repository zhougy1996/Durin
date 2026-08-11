#pragma once

#include "RHIShaderParameters.h"

namespace Durin::VulkanRHI
{
	class FVulkanCommandListContext;
	class FVulkanDevice;
	class FVulkanGraphicsPipelineState;
	class FVulkanPendingGraphicsState;

	// Descriptor state owned by one graphics PSO within a command context.
	class FVulkanGraphicsPipelineDescriptorState
	{
	public:
		explicit FVulkanGraphicsPipelineDescriptorState(FVulkanPendingGraphicsState& InOwner) : Owner(InOwner) {}
		// Retains resolved descriptor sets and dynamic offsets for one draw submission.
		struct FDescriptorSetsForDraw
		{
			const std::vector<vk::DescriptorSet>* DescriptorSets = nullptr;
			std::vector<uint32> DynamicOffsets;
		};

		~FVulkanGraphicsPipelineDescriptorState() { Reset(); }

		auto SetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void;

		auto GetOrCreateDescriptorSetsForDraw(FVulkanDevice& Device, FVulkanGraphicsPipelineState& PipelineState) -> FDescriptorSetsForDraw;

		auto ClearDescriptorSetCache() -> void;

		auto Reset() -> void;

	private:
		// Caches descriptor-set identity alongside the resolved Vulkan handle.
		struct FVulkanDescriptorSetCacheEntry
		{
			uint64 Hash = 0;
			std::vector<FRHIShaderParameterResource> Resources;
			std::vector<TRefCountPtr<FRHIResource>> ResourceOwners;
			std::vector<vk::DescriptorSet> DescriptorSets;
			uint64 LastUsed = 0;
		};

		auto CalculatePendingDescriptorHash() const -> uint64;
		auto RebuildCacheIndex() -> void;

		static auto AreDescriptorResourcesEqual(
			const std::vector<FRHIShaderParameterResource>& A,
			const std::vector<FRHIShaderParameterResource>& B
		) -> bool;

		std::vector<FRHIShaderParameterResource> PendingShaderResources;
		std::vector<TRefCountPtr<FRHIResource>> PendingResourceOwners;

		std::vector<FVulkanDescriptorSetCacheEntry> DescriptorSetCache;
		std::unordered_multimap<uint64, size_t> DescriptorSetCacheIndex;
		FVulkanPendingGraphicsState& Owner;

		friend class FVulkanPendingGraphicsState;
	};

	// Accumulates graphics bindings and applies only dirty state before a draw.
	class FVulkanPendingGraphicsState
	{
	public:
		explicit FVulkanPendingGraphicsState(FVulkanDevice& InDevice);

		~FVulkanPendingGraphicsState() { Reset(); }

		// Switches the active PSO and descriptor state, then binds the Vulkan pipeline.
		auto SetGraphicsPipelineState(FVulkanGraphicsPipelineState& InPipelineState, vk::CommandBuffer InCmdBuffer) -> void;

		auto SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void;

		auto SetScissor(float MinX, float MinY, float Width, float Height) -> void;

		auto SetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void;

		auto PrepareForDraw(FVulkanCommandListContext& InContext) -> void;

		auto ClearDescriptorSetCache() -> void;

		// Removes non-owning state before a graphics PSO can be deleted or its
		// address reused by a later pipeline.
		auto NotifyDeletedPipeline(
			FVulkanGraphicsPipelineState* PipelineState) -> void;

		auto Reset() -> void;

		auto GetPipelineState() const -> FVulkanGraphicsPipelineState* { return CurrentPipelineState; }

	private:
		friend class FVulkanGraphicsPipelineDescriptorState;
		auto SetScissorRect(uint32 MinX, uint32 MinY, uint32 Width, uint32 Height) -> void;

		auto FindOrAddDescriptorState(FVulkanGraphicsPipelineState& InPipelineState) -> FVulkanGraphicsPipelineDescriptorState&;
		auto TouchDescriptorCacheEntry(FVulkanGraphicsPipelineDescriptorState::FVulkanDescriptorSetCacheEntry& Entry) -> void;
		auto EnforceDescriptorCacheBudget() -> void;
		auto AddDescriptorCacheOccupancy(uint64 EntryCount, uint64 ValueCount) -> void;
		auto RemoveDescriptorCacheOccupancy(uint64 EntryCount, uint64 ValueCount) -> void;
		auto VerifyDescriptorCacheOccupancy() const -> void;

		FVulkanDevice& Device;

		FVulkanGraphicsPipelineState* CurrentPipelineState = nullptr;

		FVulkanGraphicsPipelineDescriptorState* CurrentDescriptorState = nullptr;

		vk::Viewport Viewport;

		vk::Rect2D Scissor;

		// Owns descriptor states by raw pointer; Reset deletes every value.
		std::unordered_map<FVulkanGraphicsPipelineState*, FVulkanGraphicsPipelineDescriptorState*> PipelineStates;
		uint64 DescriptorAccessSerial = 0;
		uint64 DescriptorEntryOccupancy = 0;
		uint64 DescriptorValueOccupancy = 0;
	};
} // namespace Durin::VulkanRHI
