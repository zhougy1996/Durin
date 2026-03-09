#include "RHICommandList.h"

#include "DynamicRHI.h"
#include "RHIContext.h"

namespace Doge
{
	FRHICommandListExecutor GCommandListExecutor;

	FRHICommandList::FRHICommandList()
	{
	}

	auto FRHICommandList::SwitchPipeline(ERHIPipeline Pipeline) -> void
	{
		if (ActivePipeline_ == Pipeline) return;

		ActivePipeline_ = Pipeline;

		switch (Pipeline)
		{
		case ERHIPipeline::Graphics:
		{
			GraphicsContext_ = GDynamicRHI->RHIGetDefaultContext();
		}
			break;
			// TODO: compute
		default:
			break;
		}
	}

	auto FRHICommandList::BeginFrame() -> void
	{
		GetContext().RHIBeginFrame();
	}

	auto FRHICommandList::EndFrame() -> void
	{
		GetContext().RHIEndFrame();
	}

	auto FRHICommandList::BeginRenderPass(const FRHIRenderPassInfo& Info, FName Name) -> void
	{
		GetContext().RHIBeginRenderPass(Info, Name);
	}

	auto FRHICommandList::EndRenderPass() -> void
	{
		GetContext().RHIEndRenderPass();
	}

	auto FRHICommandList::BeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetTexture) -> void
	{
		GetContext().RHIBeginDrawingViewport(Viewport, RenderTargetTexture);
	}

	auto FRHICommandList::EndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void
	{
		GetContext().RHIEndDrawingViewport(Viewport, bPresent, bLockToVsync);
	}

	auto FRHICommandList::SetGraphicsPipelineState(FRHIGraphicsPipelineState& State) -> void
	{
		GetContext().RHISetGraphicsPipelineState(State);
	}

	auto FRHICommandList::DrawPrimitive() -> void
	{
		GetContext().RHIDrawPrimitive();
	}

	auto FRHICommandList::SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
	{
		GetContext().RHISetViewport(MinX, MinY, MinZ, MaxX, MaxY, MaxZ);
	}

	auto FRHICommandList::SubmitCommandsHint() -> void
	{
		GetContext().RHISubmitCommandsHint();
	}

	auto FRHICommandListImmediate::SubmitAndBlockUntilGPUIdle() -> void
	{
		GDynamicRHI->RHIBlockUntilGPUIdle();
	}

	auto FRHICommandListImmediate::Get() -> FRHICommandListImmediate&
	{
		return GCommandListExecutor.GetImmediateCommandList();
	}

	FRHICommandListExecutor::FRHICommandListExecutor()
	{
	}

	auto FRHICommandListExecutor::GetImmediateCommandList() -> FRHICommandListImmediate&
	{
		return GCommandListExecutor.CommandListImmediate_;
	}
}