#include "LaunchEngineLoop.h"

#include "Threading/RunnableThread.h"
#include "DObject/DObjectGlobals.h"
#include "ApplicationCore.h"
#include "RHI.h"
#include "Mona.h"
#include "DurinEdGlobals.h"
#include "Engine/Engine.h"

#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Misc/AppConfigCache.h"
#include "Misc/Paths.h"
#include "Misc/StringConvert.h"

#include "Shader/ShaderPaths.h"
#include "DurinEngine.h"


namespace Durin
{
	FEngineLoop GEngineLoop;

	auto FEngineLoop::PreInit() -> void
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;

		FPlatformMisc::EnableUserBinaryDirectoriesSearch();
		AddDllDirectory(StringConvert::Utf8ToWide(FPaths::EngineThirdPartyRuntimeBinariesDir()).c_str());

		CoreInternal::LoadApplicationConfig(FPaths::LaunchDir() + "DurinConfig.yaml");

		FNameInit(); // Initialize FName system.
		LoggerInit();
		DURIN_DEBUG("Application name: {}", GAppConfig.GetStringValue("AppName"));
		DURIN_INFO(STR("Launching Durin engine..."));
		DURIN_DEBUG(STR("Launch directory: {}"), FPaths::LaunchDir());
		DURIN_DEBUG(STR("Engine directory: {}"), FPaths::EngineDir());
		PathUtilities::InitDefaultMountPoints(); // Initialize default mount points to enable path resolving.

		FModuleManager::Get().LoadModule("RenderCore");
		DObjectInit();
	}

	auto FEngineLoop::Init() -> void
	{
		ApplicationCoreInit();
		RHIInit();
		Mona::MonaInit();
		EditorInit();

		// Create engine instance, this is just for testing, we should have a more robust engine initialization process
		GEngine = new DEngine();
		GEngine->Init();

		InitRenderingThread();
		DURIN_INFO(STR("Durin engine initialized."));
	}

	// Called from render thread
	static auto BeginFrameRenderThread(FRHICommandListImmediate& CommandList, uint64 FrameCounter) -> void
	{
		check(IsInRenderingThread());
		GFrameCounterRenderThread = FrameCounter;
		CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		GDynamicRHI->RHIBeginFrame();
	}

	// Called from render thread
	static auto EndFrameRenderThread(FRHICommandListImmediate& RHICmdList, uint64 FrameCounter) -> void
	{
		check(IsInRenderingThread());
		check(GFrameCounterRenderThread == FrameCounter);
		GDynamicRHI->RHIEndFrame_RenderThread(RHICmdList);
	}

	auto FEngineLoop::Tick() -> void
	{
		uint64 CurrentFrameCounter = GFrameCounter;

		// Game logic.
		GEngine->Tick(0.0f, false);

		// Process application events, and paint UI.
		Mona::FMonaApplication::Get().Tick();

		if (GIsRequestingExit) return;

		Mona::NewFrame();

		// Start recording render commands for the current frame.
		ENQUEUE_RENDER_COMMAND(BeginFrame)([CurrentFrameCounter](FRHICommandListImmediate& CommandList) {
			BeginFrameRenderThread(CommandList, CurrentFrameCounter);
		});

		Mona::Render();

		ENQUEUE_RENDER_COMMAND(EndFrame)([CurrentFrameCounter](FRHICommandListImmediate& RHICmdList) {
			EndFrameRenderThread(RHICmdList, CurrentFrameCounter);
		});

		FFrameSync::Sync(FFrameSync::EFlushMode::EndFrame);
		GFrameCounter++;

		CalculateFPSTimings();
	}

	auto FEngineLoop::Exit() -> void
	{
		ShutdownRenderingThread();

		delete GEngine;

		Mona::MonaShutdown();

		GDynamicRHI->Shutdown();

		FModuleManager::Get().UnloadModulesAtShutdown();

		ApplicationCoreShutdown();
		DURIN_INFO(STR("Durin Engine exited."));
	}
} // namespace Durin