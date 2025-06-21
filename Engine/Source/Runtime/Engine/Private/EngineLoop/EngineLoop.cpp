#include "EngineLoop/EngineLoop.h"

#include "CoreGlobals.h"
#include "ApplicationCore.h"
#include "RHI.h"
#include "Mona.h"

#include "RHICommandList.h"
#include "RHIResources.h"
#include "RHIPipeline.h"

constexpr auto DLLModuleDependencies = std::array{"MainFrame"};

TSharedPtr<FRHIGraphicsPipelineState> GTestPipeline;

auto FEngineLoop::PreInit() -> void
{
	LoggerInit();
	DOGE_INFO("PreInit");
}

auto FEngineLoop::Init() -> void
{
	ApplicationInit();
	RHIInit();
	MonaInit();
	// EditorInit();

	// test code
	FGraphicsPipelineStateInitializer Initializer; // empty

	// Create pipeline
	// Render pass is created when creating pipeline
	GTestPipeline = GDynamicRHI->RHICreateGraphicsPipelineState(Initializer);

	FRHICommandList& CommandList = FRHICommandListImmediate::Get();
	// Switch to graphics pipeline, call this before any other command
	CommandList.SwitchPipeline(ERHIPipeline::eGraphics);
}

auto FEngineLoop::Tick() -> void
{
	FMonaApplication::Get().Tick();
	if (GIsRequestingExit)
	{
		return;
	}
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
	FRHIRenderPassInfo PassInfo;
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

auto FEngineLoop::Exit() -> void
{
	FMonaApplication::Get().Shutdown();
}