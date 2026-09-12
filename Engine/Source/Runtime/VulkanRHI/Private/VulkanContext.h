#pragma once

#include "RHIContext.h"
#include "VulkanMemory.h"
#include "VulkanTransferArena.h"

namespace Durin::VulkanRHI
{
	class FVulkanDynamicRHI;
	class FVulkanDevice;
	class FVulkanQueue;
	class FVulkanGraphicsPipelineState;
	class FVulkanCommandBufferManager;
	class FVulkanCommandBuffer;
	class FVulkanCommandBufferPool;
	using FVulkanCompletionToken = uint64;
	class FVulkanSemaphore;
	class FVulkanPayload;
	class FVulkanPendingGraphicsState;
	class FVulkanPendingComputeState;
	class FVulkanComputePipelineState;
	class FVulkanTexture;
	class FVulkanGPUTimingQuery;
	class FVulkanQueueTransfer;

	// Translates backend-neutral command-list operations into Vulkan command recording.
	class FVulkanCommandListContext : public IRHICommandContext
	{
	public:
		FVulkanCommandListContext(FVulkanDynamicRHI* InRHI, FVulkanDevice& InDevice, FVulkanQueue* InQueue);

		~FVulkanCommandListContext() override;
		auto RHIGetQueueContext(FRHIQueueId Id) -> IRHICommandContext* override;
		auto RHISetReplayStorageOwner(std::shared_ptr<void> Owner) -> void override;
		auto RHIBeginGPUSubmission(const FRHIGPUSubmissionDesc& Desc) -> void override;
		auto RHIEndGPUSubmission(const FRHIGPUSubmissionReceipt& Signal) -> void override;
		auto RHIReleaseQueueOwnership(const std::shared_ptr<FRHIQueueTransfer>& Transfer) -> void override;
		auto RHIAcquireQueueOwnership(const std::shared_ptr<FRHIQueueTransfer>& Transfer) -> void override;
		auto ReleaseQueueOwnership(const std::shared_ptr<FVulkanQueueTransfer>& Transfer) -> void;
		auto AcquireQueueOwnership(const std::shared_ptr<FVulkanQueueTransfer>& Transfer) -> void;

		auto RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void override;

		auto RHISetScissor(float MinX, float MinY, float Width, float Height) -> void override;
		auto RHISetDepthBias(float ConstantFactor, float Clamp,
			float SlopeFactor) -> void override;

		auto RHIBeginFrame(const FRHIBeginFrameArgs& Args) -> void override;

		auto RHISubmitCommands() -> void override;

		auto RHIEndFrame() -> void override;
		auto RHIBeginDiagnosticRegion(std::string_view Name) -> void override;
		auto RHIEndDiagnosticRegion() -> void override;
		auto RHIBeginGPUTimingQuery(FRHIGPUTimingQuery* Query) -> void override;
		auto RHIEndGPUTimingQuery(FRHIGPUTimingQuery* Query) -> void override;

		auto RHIBeginRenderPass(const FRHIRenderPassInfo& RenderPassInfo, FName Name) -> void override;

		auto RHIEndRenderPass() -> void override;

		auto RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void override;

		auto RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void override;

		auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& GraphicsPipelineState) -> void override;
		auto RHISetComputePipelineState(FRHIComputePipelineState& ComputePipelineState) -> void override;

		auto RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* InVertexBuffer, uint32 Offset) -> void override;

		auto RHIBindIndexBuffer(FRHIBuffer* InIndexBuffer, uint32 Offset) -> void override;

		auto RHITransitionBuffers(std::span<const FRHIBufferTransition> Transitions) -> void override;

		auto RHITransitionTextures(std::span<const FRHITextureTransition> Transitions) -> void override;
		auto RHICopyBuffer(FRHIBuffer* Source, FRHIBuffer* Destination,
			std::span<const FRHIBufferCopyRegion> Regions) -> void override;
		auto RHICopyBufferToTexture(FRHIBuffer* Source, FRHITexture* Destination,
			std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override;
		auto RHICopyTextureToBuffer(FRHITexture* Source, FRHIBuffer* Destination,
			std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override;
		auto RHICopyTexture(FRHITexture* Source, FRHITexture* Destination,
			std::span<const FRHITextureCopyRegion> Regions) -> void override;

		auto RHIWriteBuffer(FRHIBuffer* Buffer, uint32 Offset, FByteView Data) -> void override;

		auto RHIInitializeTexture(FRHITexture* Texture) -> void override;

		auto RHIUpdateTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, FByteView SourceData) -> void override;
		auto RHIUpdateTexture3D(FRHITexture* Texture, uint32 MipIndex,
			const FUpdateTextureRegion3D& UpdateRegion, uint32 SourceRowPitch,
			uint32 SourceDepthPitch, FByteView SourceData) -> void override;

		auto RHIReadTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, FByteBuffer& OutData) -> bool override;

		auto RHIAllocateDynamicUniformBuffer(const void* Data, uint32 Size) -> FRHIUniformBufferRange override;
		auto RHIAllocateDynamicStorageBuffer(const void* Data, uint32 Size)
			-> FRHIStorageBufferRange override;

		auto RHIAcquireBackBuffer(FRHITexture* BackBuffer) -> void override;

		auto RHIBlockUntilGPUIdle() -> void override;

		auto RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void override;

		auto RHISetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void override;

		auto RHIDraw(const FRHIDrawArguments& Arguments) -> void override;

		auto RHIDrawIndexed(const FRHIDrawIndexedArguments& Arguments) -> void override;
		auto RHIDispatch(uint32 GroupCountX, uint32 GroupCountY,
			uint32 GroupCountZ) -> void override;

		auto GetCommandBuffer() -> FVulkanCommandBuffer*;

		auto RHISetShaderUniformBuffer(FRHIShader* InShader, uint32 SetIndex, uint32 BindIndex, FRHIBuffer* InUniformBuffer) -> void;

		auto AddWaitSemaphore(vk::PipelineStageFlags InWaitFlag, FVulkanSemaphore* InWaitSemaphore) -> void;

		auto AddWaitSemaphores(vk::PipelineStageFlags InWaitFlag, std::span<FVulkanSemaphore*> InWaitSemaphores) -> void;

		auto AddSignalSemaphore(FVulkanSemaphore* InSignalSemaphore) -> void;

		auto AddSignalSemaphores(std::span<FVulkanSemaphore*> InSignalSemaphores) -> void;

		auto GetQueue() const -> FVulkanQueue* { return Queue; }

		auto NotifyDeleted_Image(vk::Image Image) -> void;

		auto NotifyDeleted_GraphicsPipeline(
			FVulkanGraphicsPipelineState* PipelineState) -> void;
		auto NotifyDeleted_ComputePipeline(
			FVulkanComputePipelineState* PipelineState) -> void;

		// Seals and transfers ownership; native submission is coordinator-owned.
		auto Finalize() -> std::unique_ptr<FVulkanPayload>;
		auto HasPendingCommands() const -> bool { return !Payloads.empty(); }
		auto RetainAllocation(std::shared_ptr<void> Owner) -> void;
		auto AcquireTransferRange(EVulkanAllocationClassCandidate AllocationClass,
			uint64 Size, uint64 Alignment) -> FVulkanTransferRange;

	protected:
		auto ValidateDrawBindings(uint32 VertexCount, uint32 InstanceCount,
			uint32 FirstVertex, uint32 FirstInstance, bool bIndexed) const -> void;
		auto PrepareNewCommandBuffer(FVulkanPayload& InPayload) -> void;

		auto GetPayload() -> FVulkanPayload&;

		FVulkanDynamicRHI* RHI = nullptr;

		FVulkanDevice& Device;

		FVulkanQueue* Queue = nullptr;

		FVulkanCommandBufferPool* Pool = nullptr;

		std::unique_ptr<FVulkanPendingGraphicsState> PendingGfxState;
		std::unique_ptr<FVulkanPendingComputeState> PendingComputeState;
		struct FPushConstantWord
		{
			EShaderStageFlags Stages;
			uint32 Offset;
			std::array<std::byte, 4> Data;
		};
		std::vector<FPushConstantWord> GraphicsPushConstants;
		std::vector<FPushConstantWord> ComputePushConstants;

		struct FBoundVertexBuffer
		{
			TRefCountPtr<FRHIBuffer> Buffer;
			uint32 Offset = 0;
		};
		std::unordered_map<uint32, FBoundVertexBuffer> BoundVertexBuffers;
		TRefCountPtr<FRHIBuffer> BoundIndexBuffer;
		uint32 BoundIndexBufferOffset = 0;

		std::vector<FVulkanPayload*> Payloads;
		std::shared_ptr<void> ReplayStorageOwner;
		bool bInsideGPUSubmission = false;
		std::vector<std::string> DiagnosticRegions;
		std::vector<FRHIGPUTimingQuery*> ActiveTimingQueries;
		// Own recorded intervals until submission transfers them to the timing
		// manager; the caller may release its query after recording ends.
		std::vector<TRefCountPtr<FVulkanGPUTimingQuery>> PendingTimingQueries;

		struct FPendingAttachmentState
		{
			FVulkanTexture* Texture = nullptr;
			FRHITextureSubresourceRange Range{};
			ERHIAccess FinalAccess = ERHIAccess::None;
		};

		std::vector<FPendingAttachmentState> PendingAttachmentStates;
	};
}
