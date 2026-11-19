#pragma once

#include "CoreMinimal.h"
#include "RHIContext.h"
#include "RHICommandList.h"
#include "Renderers/MeshDrawCommandStats.h"

namespace Durin
{
	struct FGPUCullingVisibilityQueryTestAccess
	{
		static auto Enqueue(FGPUCullingVisibilityQuery& Query, FRHICommandListImmediate& Commands, FRHIBuffer* Buffer) -> void
		{ Query.Readback.EnqueueCopy(Commands, Buffer, 4); }
		static auto Cancel(FGPUCullingVisibilityQuery& Query) -> void { Query.Readback.Cancel(); }
	};
}

namespace Durin::Testing
{
	class FVisibilityReadbackContext final : public IRHICommandContext
	{
	public:
		auto RHIBeginFrame(const FRHIBeginFrameArgs& Args) -> void override {  }
		auto RHISubmitCommands() -> void override {  }
		auto RHIEndFrame() -> void override {  }
		auto RHIBeginDiagnosticRegion(std::string_view Name) -> void override {  }
		auto RHIEndDiagnosticRegion() -> void override {  }
		auto RHIBeginRenderPass(const FRHIRenderPassInfo& InInfo, FName InName) -> void override {  }
		auto RHIEndRenderPass() -> void override {  }
		auto RHIBeginDrawingViewport(FRHIViewport* InViewport, FRHITexture* InRenderTargetRHI) -> void override {  }
		auto RHIEndDrawingViewport(FRHIViewport* InViewport, bool bInPresent, bool bInLockToVsync) -> void override {  }
		auto RHISetViewport(float InMinX, float InMinY, float InMinZ, float InMaxX, float InMaxY, float InMaxZ) -> void override {  }
		auto RHISetScissor(float InMinX, float InMinY, float InWidth, float InHeight) -> void override {  }
		auto RHISetDepthBias(float InConstantFactor, float InClamp, float InSlopeFactor) -> void override {  }
		auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& InGraphicsPipelineState) -> void override {  }
		auto RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset) -> void override {  }
		auto RHIBindIndexBuffer(FRHIBuffer* IndexBuffer, uint32 Offset) -> void override {  }
		auto RHITransitionBuffers(std::span<const FRHIBufferTransition> Transitions) -> void override {  }
		auto RHITransitionTextures(std::span<const FRHITextureTransition> Transitions) -> void override {  }
		auto RHICopyBuffer(FRHIBuffer* Source, FRHIBuffer* Destination, std::span<const FRHIBufferCopyRegion> Regions) -> void override {  }
		auto RHICopyBufferToTexture(FRHIBuffer* Source, FRHITexture* Destination, std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override {  }
		auto RHICopyTextureToBuffer(FRHITexture* Source, FRHIBuffer* Destination, std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override {  }
		auto RHICopyTexture(FRHITexture* Source, FRHITexture* Destination, std::span<const FRHITextureCopyRegion> Regions) -> void override {  }
		auto RHIWriteBuffer(FRHIBuffer* Buffer, uint32 Offset, FByteView Data) -> void override {  }
		auto RHIInitializeTexture(FRHITexture* Texture) -> void override {  }
		auto RHIUpdateTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, FByteView SourceData) -> void override {  }
		auto RHIUpdateTexture3D(FRHITexture* Texture, uint32 MipIndex, const FUpdateTextureRegion3D& UpdateRegion, uint32 SourceRowPitch, uint32 SourceDepthPitch, FByteView SourceData) -> void override {  }
		auto RHIReadTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, FByteBuffer& OutData) -> bool override { return false; }
		auto RHIAcquireBackBuffer(FRHITexture* BackBuffer) -> void override {  }
		auto RHIBlockUntilGPUIdle() -> void override { ++GPUIdleWaits; }
		uint32 GPUIdleWaits = 0;
		auto RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void override {  }
		auto RHISetShaderParameters(FRHIShader* InShader, const std::span<const FRHIShaderParameterResource>& InResourceParameters) -> void override {  }
		auto RHIDraw(const FRHIDrawArguments& Arguments) -> void override {  }
		auto RHIDrawIndexed(const FRHIDrawIndexedArguments& Arguments) -> void override {  }
		auto RHIEnqueueBufferReadback(FRHIBuffer*, uint32, uint32, std::shared_ptr<RHIPrivate::FReadbackState> Request) -> void override
		{ Pending = std::move(Request); }
		std::shared_ptr<RHIPrivate::FReadbackState> Pending;
	};

	inline auto PublishVisibilityReadback(FGPUCullingVisibilityQuery& Query, FByteBuffer Payload, bool bFail = false, bool bCancel = false) -> void
	{
		FVisibilityReadbackContext Context;
		FRHICommandListExecutor Executor(Context);
		auto Buffer = MakeRefCount<FRHIBuffer>(FRHIBufferCreateDesc::Create("Visibility", 4, 4, EBufferUsageFlags::SourceCopy));
		FGPUCullingVisibilityQueryTestAccess::Enqueue(Query, Executor.GetImmediateCommandList(), Buffer);
		if (bCancel) FGPUCullingVisibilityQueryTestAccess::Cancel(Query);
		Executor.Submit({}, ERHISubmitFlags::None);
		require(Context.Pending);
		if (bFail) Context.Pending->Fail(); else Context.Pending->Complete(std::move(Payload));
	}
}
