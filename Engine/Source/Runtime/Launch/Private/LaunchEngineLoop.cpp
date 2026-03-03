#include "LaunchEngineLoop.h"

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
#include "RHIPipeline.h"

namespace Doge
{
	constexpr auto DLLModuleDependencies = std::array{"MainFrame"};

	// TODO: move this to a more appropriate place
	TSharedPtr<FRHIGraphicsPipelineState> GTestPipeline;

	auto FEngineLoop::PreInit() -> void
	{
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
	}

	static auto CreateTestPipeline()
	{
		const TSharedPtr<Mona::MWindow> Window = Mona::FMonaApplication::Get().GetActiveTopLevelWindow();
		const FRHIViewport* Viewport = Window->GetRHIViewport().get();

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
		// Window and viewport
		TSharedPtr<Mona::MWindow> Window = Mona::FMonaApplication::Get().GetActiveTopLevelWindow();
		FRHIViewport* Viewport = Window->GetRHIViewport().get();

		FRHICommandList& CommandList = FRHICommandListImmediate::Get();
		// Draw viewport
		// Wait submit fence before acquiring image
		CommandList.BeginDrawingViewport(Viewport, nullptr);

		// Acquire image
		TSharedPtr<FRHITexture> BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(Viewport);

		Window->Paint();

		// Render pass
		FRHIRenderPassInfo PassInfo{};
		PassInfo.ColorRenderTargets[0] = BackBuffer.get();

		CommandList.BeginFrame();

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

		CommandList.EndFrame();
	}

	auto FEngineLoop::Tick() -> void
	{
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

		// Render Scene.
		// TODO
		DrawTriangle();

		// Render UI.
		// TODO

		// Execute RHI commands, this is just for testing, we should have a RHI thread and a more robust rendering process
	}

	auto FEngineLoop::Exit() -> void
	{
		// TODO: this is just for testing, we should have a more robust shutdown process
		delete GEngine;
		Mona::FMonaApplication::Get().Shutdown();
	}
}