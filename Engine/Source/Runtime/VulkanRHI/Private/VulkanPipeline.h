#pragma once

#include "VulkanCommon.h"
#include "VulkanDescriptorSets.h"
#include "VulkanShader.h"
#include "RHIPipelineCreation.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanRenderPass;
	class FVulkanCommandListContext;
	class FVulkanShader;
	class FVulkanBuffer;
	class FVulkanTexture;
	class FVulkanSampler;

	using FVulkanGraphicsPipelineInputs = FRHIGraphicsPipelineCreationInputs;
	using FVulkanComputePipelineInputs = FRHIComputePipelineCreationInputs;

	struct FVulkanPipelineDependencies
	{
		// Render-pass handles remain owned by the device's resident cache.
		const FVulkanRenderPass* RenderPass = nullptr;
		std::shared_ptr<FVulkanLayout> Layout;
	};

	// Owns one complete Vulkan compute pipeline and the layout used to bind it.
	class FVulkanComputePipelineState : public FRHIComputePipelineState
	{
	public:
		FVulkanComputePipelineState(FVulkanDevice& InDevice,
			const FVulkanComputePipelineInputs& Inputs, FComputePipelineStateKey InKey,
			FVulkanPipelineDependencies Dependencies);
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
		std::shared_ptr<void> MetadataReservation;
		FComputePipelineStateKey Key;
		friend class FVulkanPipelineManager;
	};

	// Owns one complete Vulkan graphics pipeline and the layout used to bind it.
	class FVulkanGraphicsPipelineState : public FRHIGraphicsPipelineState
	{
	public:
		FVulkanGraphicsPipelineState(FVulkanDevice& InDevice,
			const FVulkanGraphicsPipelineInputs& Inputs, FGraphicsPipelineStateKey InKey,
			FVulkanPipelineDependencies Dependencies);

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
		std::shared_ptr<void> MetadataReservation;

		FGraphicsPipelineStateKey Key;

		friend class FVulkanPipelineManager;
	};

	// Owns cache lookup/publication and acquires candidate dependencies explicitly.
	class FVulkanPipelineManager
	{
	public:
		FVulkanPipelineManager(FVulkanDevice& InDevice);
		~FVulkanPipelineManager();

		auto FindGraphicsPipelineState(const FGraphicsPipelineStateKey& Key) -> FGraphicsPipelineStateRHIRef;
		auto FindComputePipelineState(const FComputePipelineStateKey& Key) -> FComputePipelineStateRHIRef;
		VULKANRHI_API auto GetOrCreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer,
			FGraphicsPipelineStateKey Key, std::string_view DebugName)
			-> TRefCountPtr<FVulkanGraphicsPipelineState>;
		VULKANRHI_API auto GetOrCreateComputePipelineState(const FComputePipelineStateInitializer& Initializer,
			FComputePipelineStateKey Key, std::string_view DebugName)
			-> TRefCountPtr<FVulkanComputePipelineState>;

		auto FindOrAddLayout(const FVulkanDescriptorSetsLayoutInfo& LayoutInfo) -> std::shared_ptr<FVulkanLayout>;
		// Every native access to the shared driver cache uses this exclusive domain.
		// Callers must not hold a cache-table lock while compiling.
		auto CompileGraphicsPipeline(const vk::GraphicsPipelineCreateInfo& Info)
			-> vk::ResultValue<vk::Pipeline>;
		auto CompileComputePipeline(const vk::ComputePipelineCreateInfo& Info)
			-> vk::ResultValue<vk::Pipeline>;
	private:
		auto AcquireGraphicsDependencies(const FVulkanGraphicsPipelineInputs& Inputs)
			-> FVulkanPipelineDependencies;
		auto AcquireComputeDependencies(const FVulkanComputePipelineInputs& Inputs)
			-> FVulkanPipelineDependencies;
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

		FVulkanDevice& Device;

		std::unordered_map<FVulkanDescriptorSetsLayoutInfo, FLayoutCacheEntry> LayoutMap;
		std::unordered_map<FGraphicsPipelineStateKey, FPipelineCacheEntry, FGraphicsPipelineStateKeyHasher> GraphicsPipelineMap;
		std::unordered_map<FComputePipelineStateKey, FComputePipelineCacheEntry,
			FComputePipelineStateKeyHasher> ComputePipelineMap;
		vk::PipelineCache DriverPipelineCache{};
		std::mutex DriverCacheMutex;
		std::mutex CreationMutex;
		std::mutex LayoutMutex;
		uint64 AccessSerial = 0;
	};

	// Owns the driver pipeline cache used to accelerate Vulkan pipeline creation.
	class FPipelineCache
	{
	public:
		FVulkanDescriptorSetLayoutCache DSetLayoutMap;
	};
}
