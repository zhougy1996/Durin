#include "LaunchEngineLoop.h"

#include "CoreGlobals.h"
#include "DObject/Object.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/ConfigCacheJson.h"
#include "ApplicationCore.h"
#include "RHI.h"
#include "Mona.h"
#include "DogeEdGlobals.h"

#include "RHICommandList.h"
#include "RHIResources.h"
#include "RHIPipeline.h"

#include "Actors/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"

namespace Doge
{
	constexpr auto DLLModuleDependencies = std::array{"MainFrame"};

	// TODO: move this to a more appropriate place
	TSharedPtr<FRHIGraphicsPipelineState> GTestPipeline;

	auto FEngineLoop::PreInit() -> void
	{
		std::filesystem::path WorkingDir = std::filesystem::current_path();
		DOGE_DEBUG(STR("Working directory: {}"), WorkingDir.string());
		FConfigCacheJson::LoadAndParseConfig();
		LoggerInit();
		FNameInit();
		DObjectInit();

		AActor* TestActor = NewObject<AStaticMeshActor>(nullptr, "AStaticMeshActor");
	}

	auto FEngineLoop::Init() -> void
	{
		ApplicationInit();
		RHIInit();
		MonaInit();
		EditorInit();

		DOGE_DEBUG(STR("DogeEd initialized"));
	}

	static auto CreateTestPipeline()
	{
		TSharedPtr<MWindow> Window = FMonaApplication::Get().GetActiveTopLevelWindow();
		FRHIViewport* Viewport = Window->GetRHIViewport().get();

		FGraphicsPipelineStateInitializer Initializer;
		Initializer.RenderPassName = "TestRenderPass";
		Initializer.PixelFormat = Viewport->GetFormat();
		// Create pipeline
		// Render pass is created when creating pipeline
		GTestPipeline = GDynamicRHI->RHICreateGraphicsPipelineState(Initializer);
		FRHICommandList& CommandList = FRHICommandListImmediate::Get();
		// Switch to graphics pipeline, call this before any other command
		CommandList.SwitchPipeline(ERHIPipeline::eGraphics);
	}

	static auto DrawTriangle()
	{
		// Window and viewport
		TSharedPtr<MWindow> Window = FMonaApplication::Get().GetActiveTopLevelWindow();
		FRHIViewport* Viewport = Window->GetRHIViewport().get();

		FRHICommandList& CommandList = FRHICommandListImmediate::Get();
		// Draw viewport
		// Wait submit fence before acquiring image
		CommandList.BeginDrawingViewport(Viewport, nullptr);

		// Acquire image
		TSharedPtr<FRHITexture> BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(Viewport);

		// Render pass
		FRHIRenderPassInfo PassInfo{};
		PassInfo.ColorRenderTargets[0] = BackBuffer.get();

		CommandList.BeginFrame();

		CommandList.BeginRenderPass(PassInfo, "TestRenderPass");

		CommandList.SetGraphicsPipelineState(*GTestPipeline);

		CommandList.TestCommandRecord();
		// Draw call
		CommandList.DrawPrimitive();

		CommandList.EndRenderPass();

		// End drawing viewport and present
		CommandList.EndDrawingViewport(Viewport, true, false);

		CommandList.EndFrame();
	}

	auto FEngineLoop::Tick() -> void
	{
		FMonaApplication::Get().Tick();

		if (GIsRequestingExit)
		{
			return;
		}

		if (!GTestPipeline)
		{
			CreateTestPipeline();
		}

		DrawTriangle();
	}

	auto FEngineLoop::Exit() -> void
	{
		FMonaApplication::Get().Shutdown();
	}
}