#pragma once

#include "RHIPipeline.h"

namespace Doge
{
	class IRHICommandContext;
	struct FRHIRenderPassInfo;
	class FRHICommandListExecutor;
	class FRHITexture;
	class FRHIViewport;

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

		auto GetContext() const -> IRHICommandContext& { return *GraphicsContext_; }

		auto SubmitCommandsHint() -> void;

	private:
		ERHIPipeline ActivePipeline_ = ERHIPipeline::eNone;

		IRHICommandContext* GraphicsContext_ = nullptr;
	};

	// Singleton command list
	class RHI_API FRHICommandListImmediate : public FRHICommandList
	{
	public:
		static auto Get() -> FRHICommandListImmediate&;
	};

	class RHI_API FRHICommandListExecutor
	{
	public:
		FRHICommandListExecutor();

		static auto GetImmediateCommandList() -> FRHICommandListImmediate&;

	private:
		FRHICommandListImmediate CommandListImmediate_;
	};

	extern RHI_API FRHICommandListExecutor GCommandListExecutor;
}