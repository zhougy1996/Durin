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

	// Owns a Vulkan graphics pipeline and the descriptor layout used to bind it.
	class FVulkanGraphicsPipelineState : public FRHIGraphicsPipelineState
	{
	public:
		FVulkanGraphicsPipelineState(FVulkanDevice& InDevice, const FGraphicsPipelineStateInitializer& Initializer);

		~FVulkanGraphicsPipelineState() override;

		auto Bind(vk::CommandBuffer InCmdBuffer) -> void;

		auto GetPipelineLayout() const -> vk::PipelineLayout { return PipelineLayout; }

		auto GetDescriptorSetsLayout() const -> const FVulkanDescriptorSetsLayout&;
		auto GetRenderTargetLayout() const -> const FRHIRenderTargetLayout& { return RenderTargetLayout; }

		auto PushConstants(FVulkanCommandListContext& InContext, EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* pValues) const -> void;

	protected:
		const FVulkanRenderPass* RenderPass = nullptr;
		FRHIRenderTargetLayout RenderTargetLayout{};

		auto KeepShadersAlive() -> void;

		auto ReleaseShaders() -> void;

		FVulkanDevice& Device;

		FVulkanShader* Shaders[EShaderStage::Count] = { nullptr };

		FVulkanLayout* Layout = nullptr;

		vk::PipelineLayout PipelineLayout{};

		vk::Pipeline Pipeline{};

		friend class FVulkanGraphicsPipelineState;
		friend class FVulkanPipelineStateCacheManager;
	};

	// Creates and reuses named Vulkan graphics pipeline state objects.
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

	// Owns the driver pipeline cache used to accelerate Vulkan pipeline creation.
	class FPipelineCache
	{
	public:
		FVulkanDescriptorSetLayoutCache DSetLayoutMap;
	};
}
