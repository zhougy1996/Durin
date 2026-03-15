#include "LaunchEngineLoop.h"

#include "HAL/RunnableThread.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/ConfigCacheJson.h"
#include "ApplicationCore.h"
#include "RHI.h"
#include "Mona.h"
#include "DogeEdGlobals.h"
#include "Engine/Engine.h"

#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderingThread.h"

namespace Doge
{
	FEngineLoop GEngineLoop;

	constexpr auto DLLModuleDependencies = std::array{"MainFrame"};

	// TODO: move this to a more appropriate place
	TRefCountPtr<FRHIGraphicsPipelineState> GTestPipeline;

	auto FEngineLoop::PreInit() -> void
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;

		GWorkDirectory = std::filesystem::current_path();
		DOGE_DEBUG(STR("Working directory: {}"), GWorkDirectory.string());
		FConfigCacheJson::LoadAndParseConfig();

		LoggerInit();
		FNameInit();
		DObjectInit();
	}

	auto FEngineLoop::Init() -> void
	{
		ApplicationInit();
		RHIInit();
		Mona::MonaInit();
		EditorInit();

		// Create engine instance, this is just for testing, we should have a more robust engine initialization process
		GEngine = new DEngine();
		GEngine->Init();

		InitRenderingThread();
	}

	static auto BeginFrameRenderThread(FRHICommandListImmediate& CommandList, uint64 FrameCounter) -> void
	{
		GFrameCounterRenderThread = FrameCounter;
		CommandList.BeginFrame();
	}

	static auto EndFrameRenderThread(FRHICommandListImmediate& CommandList, uint64 FrameCounter) -> void
	{
		CommandList.EndFrame();
		GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
	}

	static auto CreateTestPipeline()
	{
		auto& App = Mona::FMonaApplication::Get();
		const auto Renderer = dynamic_cast<Mona::FMonaRHIRenderer*>(App.GetRenderer());
		const std::shared_ptr<Mona::MWindow> MainWindow = App.GetActiveTopLevelWindow();
		const FRHIViewport* Viewport = Renderer->GetRHIViewport(*MainWindow).get();

		FGraphicsPipelineStateInitializer Initializer;
		Initializer.RenderPassName = "TestRenderPass";
		Initializer.PixelFormat = Viewport->GetFormat();
		// Create pipeline
		// Render pass is created when creating pipeline
		GTestPipeline = GDynamicRHI->RHICreateGraphicsPipelineState(Initializer);
		FRHICommandList& CommandList = FRHICommandListImmediate::Get();
		// Switch to graphics pipeline, call this before any other command
		CommandList.SwitchPipeline(ERHIPipeline::Graphics);
	}

	static auto DrawTriangle()
	{
		auto& App = Mona::FMonaApplication::Get();
		const auto Renderer = dynamic_cast<Mona::FMonaRHIRenderer*>(App.GetRenderer());
		const std::shared_ptr<Mona::MWindow> MainWindow = App.GetActiveTopLevelWindow();
		TSharedPtr<FRHIViewport> SharedViewport = Renderer->GetRHIViewport(*MainWindow);

		ENQUEUE_RENDER_COMMAND(TestDrawTriangle)([SharedViewport](FRHICommandListImmediate& CommandList)
		{
			FRHIViewport* Viewport = SharedViewport.get();
			// Draw viewport
			// Wait submit fence before acquiring image
			CommandList.BeginDrawingViewport(Viewport, nullptr);

			// Acquire image
			TRefCountPtr<FRHITexture> BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(Viewport);

			FRHIRenderPassInfo PassInfo{};
			PassInfo.ColorRenderTargets[0] = BackBuffer.GetReference();

			CommandList.BeginRenderPass(PassInfo, "TestRenderPass");

			CommandList.SetGraphicsPipelineState(*GTestPipeline);

			auto Width = BackBuffer->GetSizeX();
			auto Height = BackBuffer->GetSizeY();
			CommandList.SetViewport(0, 0, 0, static_cast<float>(Width), static_cast<float>(Height), 1.0f);

			// Draw call
			CommandList.DrawPrimitive();

			CommandList.EndRenderPass();

			// End drawing viewport and present
			CommandList.EndDrawingViewport(Viewport, true, false);
		});
	}

	auto FEngineLoop::Tick() -> void
	{
		uint64 CurrentFrameCounter = GFrameCounter;

		// Game logic.
		GEngine->Tick(0.0f, false);

		if (!GTestPipeline)
		{
			CreateTestPipeline();
		}

		// Process application events, and paint UI.
		Mona::FMonaApplication::Get().Tick();
		if (GIsRequestingExit)
		{
			return;
		}

		ENQUEUE_RENDER_COMMAND(BeginFrame)([CurrentFrameCounter](FRHICommandListImmediate& CommandList)
		{
			BeginFrameRenderThread(CommandList, CurrentFrameCounter);
		});

		// Render Scene.
		DrawTriangle();

		ENQUEUE_RENDER_COMMAND(EndFrame)([CurrentFrameCounter](FRHICommandListImmediate& RHICmdList)
		{
			EndFrameRenderThread(RHICmdList, CurrentFrameCounter);
		});

		FFrameSync::Sync(FFrameSync::EFlushMode::EndFrame);
		GFrameCounter++;
	}

	auto FEngineLoop::Exit() -> void
	{
		ShutdownRenderingThread();
		// TODO: this is just for testing, we should have a more robust shutdown process
		delete GEngine;
		Mona::FMonaApplication::Shutdown();
	}
}