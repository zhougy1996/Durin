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

	// Owns one complete Vulkan compute pipeline and the layout used to bind it.
	class FVulkanComputePipelineState : public FRHIComputePipelineState
	{
	public:
		FVulkanComputePipelineState(FVulkanDevice& InDevice,
			const FComputePipelineStateInitializer& Initializer,
			FComputePipelineStateKey InKey, std::string_view DebugName);
		~FVulkanComputePipelineState() override;

		auto Bind(vk::CommandBuffer InCmdBuffer) -> void;
		auto GetPipelineLayout() const -> vk::PipelineLayout { return PipelineLayout; }
		auto GetDescriptorSetsLayout() const -> const FVulkanDescriptorSetsLayout&;
		auto GetKey() const -> const FComputePipelineStateKey& { return Key; }
		auto PushConstants(FVulkanCommandListContext& InContext,
			EShaderStageFlags StageFlags, uint32 Offset, uint32 Size,
			const void* Values) const -> void;

	private:
		FVulkanDevice& Device;
		std::shared_ptr<FVulkanLayout> Layout;
		vk::PipelineLayout PipelineLayout{};
		vk::Pipeline Pipeline{};
		FComputePipelineStateKey Key;
	};

	// Owns one complete Vulkan graphics pipeline and the layout used to bind it.
	class FVulkanGraphicsPipelineState : public FRHIGraphicsPipelineState
	{
	public:
		FVulkanGraphicsPipelineState(FVulkanDevice& InDevice,
			const FGraphicsPipelineStateInitializer& Initializer,
			FGraphicsPipelineStateKey InKey, std::string_view DebugName);

		~FVulkanGraphicsPipelineState() override;

		auto Bind(vk::CommandBuffer InCmdBuffer) -> void;

		auto GetPipelineLayout() const -> vk::PipelineLayout { return PipelineLayout; }

		auto GetDescriptorSetsLayout() const -> const FVulkanDescriptorSetsLayout&;
		auto GetKey() const -> const FGraphicsPipelineStateKey& { return Key; }
		auto PushConstants(FVulkanCommandListContext& InContext, EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* pValues) const -> void;

	protected:
		const FVulkanRenderPass* RenderPass = nullptr;

		FVulkanDevice& Device;

		std::shared_ptr<FVulkanLayout> Layout;

		vk::PipelineLayout PipelineLayout{};

		vk::Pipeline Pipeline{};

		FGraphicsPipelineStateKey Key;

		friend class FVulkanPipelineManager;
	};

	// Creates graphics pipelines and owns reusable structural layout data.
	class FVulkanPipelineManager
	{
	public:
		FVulkanPipelineManager(FVulkanDevice& InDevice);
		~FVulkanPipelineManager();

		auto CreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer,
			FGraphicsPipelineStateKey Key, std::string_view DebugName)
			-> TRefCountPtr<FVulkanGraphicsPipelineState>;
		auto CreateComputePipelineState(const FComputePipelineStateInitializer& Initializer,
			FComputePipelineStateKey Key, std::string_view DebugName)
			-> TRefCountPtr<FVulkanComputePipelineState>;

		auto FindOrAddLayout(const FVulkanDescriptorSetsLayoutInfo& LayoutInfo) -> std::shared_ptr<FVulkanLayout>;
		auto GetDriverPipelineCache() const -> vk::PipelineCache { return DriverPipelineCache; }
	private:
		struct FLayoutCacheEntry
		{
			std::shared_ptr<FVulkanLayout> Layout;
			uint64 LastUsed = 0;
		};
		struct FPipelineCacheEntry
		{
			TRefCountPtr<FVulkanGraphicsPipelineState> Pipeline;
			uint64 LastUsed = 0;
		};
		struct FComputePipelineCacheEntry
		{
			TRefCountPtr<FVulkanComputePipelineState> Pipeline;
			uint64 LastUsed = 0;
		};

		auto InitializeDriverPipelineCache() -> void;
		auto SaveDriverPipelineCache() -> void;
		auto EvictLayoutIfNeeded() -> bool;
		auto EvictPipelineIfNeeded() -> bool;
		auto EvictComputePipelineIfNeeded() -> bool;

		FVulkanDevice& Device;

		std::unordered_map<FVulkanDescriptorSetsLayoutInfo, FLayoutCacheEntry> LayoutMap;
		std::unordered_map<FGraphicsPipelineStateKey, FPipelineCacheEntry, FGraphicsPipelineStateKeyHasher> GraphicsPipelineMap;
		std::unordered_map<FComputePipelineStateKey, FComputePipelineCacheEntry,
			FComputePipelineStateKeyHasher> ComputePipelineMap;
		vk::PipelineCache DriverPipelineCache{};
		uint64 AccessSerial = 0;
	};

	// Owns the driver pipeline cache used to accelerate Vulkan pipeline creation.
	class FPipelineCache
	{
	public:
		FVulkanDescriptorSetLayoutCache DSetLayoutMap;
	};
}
