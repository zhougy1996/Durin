#include "LaunchFrame.h"

#include "Engine/Engine.h"
#include "EngineGlobals.h"
#include "Mona.h"
#include "Profiling/Profiling.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		auto BeginFrameRenderThread(
			FRHICommandListImmediate& CommandList,
			uint64 LogicFrameCounter,
			uint64 RenderFrameCounter) -> void
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("RenderFrame.Begin");
			check(IsInRenderingThread());
			GFrameCounterRenderThread = LogicFrameCounter;
			GRenderFrameCounterRenderThread = RenderFrameCounter;
			CommandList.SwitchPipeline(ERHIPipeline::Graphics);
			GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
		}

		auto EndFrameRenderThread(
			FRHICommandListImmediate& RHICmdList,
			uint64 LogicFrameCounter,
			uint64 RenderFrameCounter) -> void
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("RenderFrame.End");
			check(IsInRenderingThread());
			check(GFrameCounterRenderThread == LogicFrameCounter);
			check(GRenderFrameCounterRenderThread == RenderFrameCounter);
			GDynamicRHI->RHIEndFrame_RenderThread(RHICmdList);
		}
	}

	auto RenderEngineFrame() -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.RenderFrame");
		if (GDynamicRHI == nullptr) return;

		const uint64 LogicFrameCounter = GFrameCounter;
		const uint64 RenderFrameCounter = GRenderFrameCounter;
		ENQUEUE_RENDER_COMMAND(BeginFrame)(
			[LogicFrameCounter, RenderFrameCounter](FRHICommandListImmediate& CommandList) {
				BeginFrameRenderThread(CommandList, LogicFrameCounter, RenderFrameCounter);
			});

		// UI drawing may enqueue offscreen preview work that allocates frame-local
		// uniform ranges, so admit it only after the frame-begin boundary.
		Mona::NewFrame();
		if (GEngine != nullptr) GEngine->RedrawViewports();
		Mona::Render();

		ENQUEUE_RENDER_COMMAND(EndFrame)(
			[LogicFrameCounter, RenderFrameCounter](FRHICommandListImmediate& RHICmdList) {
				EndFrameRenderThread(RHICmdList, LogicFrameCounter, RenderFrameCounter);
			});
		FFrameSync::Sync(FFrameSync::EFlushMode::EndFrame);
		GRenderFrameCounter++;
	}
}
