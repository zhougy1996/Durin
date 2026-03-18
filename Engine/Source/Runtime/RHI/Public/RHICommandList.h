#pragma once

#include "DynamicRHI.h"
#include "RHIDefinitions.h"
#include "RHIResources.h"

namespace Doge
{
	struct FRHITextureCreateDesc;
	struct FRHIRenderPassInfo;

	class FRHITexture;
	class IRHICommandContext;
	class FRHICommandListExecutor;
	class FRHIViewport;
	class FRHIGraphicsPipelineState;

	class RHI_API FRHICommandList
	{
	public:
		FRHICommandList();

		virtual ~FRHICommandList() = default;

		// Call this function to switch between graphics and compute pipelines
		// This function will set the context, so call this before any other command
		auto SwitchPipeline(ERHIPipeline Pipeline) -> void;

		auto BeginFrame() -> void;

		auto EndFrame() -> void;

		auto BeginRenderPass(const FRHIRenderPassInfo& Info, FName Name) -> void;

		auto EndRenderPass() -> void;

		auto BeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetTexture) -> void;

		auto EndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void;

		auto SetGraphicsPipelineState(FRHIGraphicsPipelineState& State) -> void;

		auto DrawPrimitive() -> void;

		auto SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void;

		auto GetContext() const -> IRHICommandContext& { return *GraphicsContext; }

		auto SubmitCommandsHint() -> void;

		auto CreateBuffer(const FRHIBufferCreateDesc& InCreateDesc) -> TRefCountPtr<FRHIBuffer>;

	private:
		ERHIPipeline ActivePipeline = ERHIPipeline::None;

		IRHICommandContext* GraphicsContext = nullptr;
	};

	// Main command list class that will be used to record commands, and submit to GPU immediately when calling EndFrame
	class RHI_API FRHICommandListImmediate : public FRHICommandList
	{
	public:
		auto SubmitAndBlockUntilGPUIdle() -> void;

		static auto Get() -> FRHICommandListImmediate&;
	};

	class RHI_API FRHICommandListExecutor
	{
	public:
		FRHICommandListExecutor();

		static auto GetImmediateCommandList() -> FRHICommandListImmediate&;

	private:
		FRHICommandListImmediate CommandListImmediate;
	};

	extern RHI_API FRHICommandListExecutor GCommandListExecutor;

	FORCEINLINE TRefCountPtr<FRHITexture> RHICreateTexture(const FRHITextureCreateDesc& CreateDesc)
	{
		return GDynamicRHI->RHICreateTexture(FRHICommandListImmediate::Get(), CreateDesc);
	}
}