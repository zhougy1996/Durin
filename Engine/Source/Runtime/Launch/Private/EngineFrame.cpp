#include "EngineFrame.h"
#include "Diagnostics/SkyLightingRuntimeSmoke.h"
#include "IRendererModule.h"

#include "Engine/Engine.h"
#include "Engine/EngineGlobals.h"
#include "Misc/Time.h"
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

	namespace
	{
		auto RenderFrame(EEngineFrameMode Mode) -> void
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.RenderFrame");
			if (GDynamicRHI == nullptr)
			{
				RecordEngineFrameRenderTimings(0.0f, 0.0f, 0.0f, 0.0f);
				return;
			}

			const uint64 LogicFrameCounter = GFrameCounter;
			const uint64 RenderFrameCounter = GRenderFrameCounter;
			auto* Renderer = GEngine ? GEngine->GetRendererModule() : nullptr;
			ENQUEUE_RENDER_COMMAND(BeginFrame)(
				[LogicFrameCounter, RenderFrameCounter, Renderer](FRHICommandListImmediate& CommandList) {
					BeginFrameRenderThread(CommandList, LogicFrameCounter, RenderFrameCounter);
					BeginSkyLightingSmokeFrame(CommandList);
					if (Renderer) Renderer->UpdateScenes_RenderThread(CommandList);
					AfterSkyLightingSmokeUpdate(CommandList);
				});

			const double UIFrameBuildStarted = FTime::Seconds();
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.UIFrameBuild");
				Mona::NewFrame();
				// UI construction determines viewport sizes and visibility before scene submission.
				if (Mona::GetActiveUIBackend() != nullptr)
					Mona::FMonaApplication::Get().DrawWindows();
			}
			const double SceneSubmissionStarted = FTime::Seconds();
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.SceneSubmission");
				if (ShouldRedrawEngineViewports(Mode) && GEngine != nullptr)
					GEngine->RedrawViewports();
			}
			const double UISubmissionStarted = FTime::Seconds();
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.UISubmission");
				Mona::Render();
			}
			const double UISubmissionEnded = FTime::Seconds();

			ENQUEUE_RENDER_COMMAND(EndFrame)(
				[LogicFrameCounter, RenderFrameCounter](FRHICommandListImmediate& RHICmdList) {
					EndSkyLightingSmokeFrame(RHICmdList);
					EndFrameRenderThread(RHICmdList, LogicFrameCounter, RenderFrameCounter);
				});
			const double SyncStarted = FTime::Seconds();
			FFrameSync::Sync(FFrameSync::EFlushMode::EndFrame);
			RecordEngineFrameRenderTimings(
				static_cast<float>((SceneSubmissionStarted - UIFrameBuildStarted) * 1000.0),
				static_cast<float>((UISubmissionStarted - SceneSubmissionStarted) * 1000.0),
				static_cast<float>((UISubmissionEnded - UISubmissionStarted) * 1000.0),
				static_cast<float>((FTime::Seconds() - SyncStarted) * 1000.0));
			GRenderFrameCounter++;
		}
	}

	auto RenderEngineFrame() -> void
	{
		RenderFrame(EEngineFrameMode::Running);
	}

	auto RenderEngineStartupFrame() -> void
	{
		RenderFrame(EEngineFrameMode::Startup);
	}
}
