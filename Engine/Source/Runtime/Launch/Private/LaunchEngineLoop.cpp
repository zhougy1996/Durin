#include "LaunchEngineLoop.h"

#include "Threading/QueuedThreadPool.h"
#include "Threading/RunnableThread.h"
#include "DObject/DObjectGlobals.h"
#include "ApplicationCore.h"
#include "RHI.h"
#include "Mona.h"
#include "Engine/Engine.h"

#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Misc/AppConfigCache.h"
#include "Misc/Paths.h"
#include "Misc/StringConvert.h"

#include "Shader/ShaderPaths.h"
#include "DurinEngine.h"

#if DURIN_WITH_EDITOR
	#include "Editor/EditorEngine.h"
#else
	#include "Engine/GameEngine.h"
#endif


namespace Durin
{
	namespace
	{
		constexpr auto GetAppConfigFileName() -> std::string_view
		{
			return DURIN_PROFILE_NAME ".yaml";
		}

	} // namespace

	FEngineLoop GEngineLoop;

	auto FEngineLoop::PreInit() -> void
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;

		FPlatformMisc::EnableUserBinaryDirectoriesSearch();
		AddDllDirectory(String::Utf8ToWide(FPaths::EngineThirdPartyRuntimeBinariesDir()).c_str());

		CoreInternal::LoadApplicationConfig(FPaths::LaunchDir() + std::string(GetAppConfigFileName()));

		FNameInit(); // Initialize FName system.
		LoggerInit();
		DURIN_DEBUG("Application name: {}", GAppConfig.GetView("AppName").GetString());
		DURIN_INFO(STR("Launching Durin engine..."));
		DURIN_DEBUG(STR("Launch directory: {}"), FPaths::LaunchDir());
		DURIN_DEBUG(STR("Engine directory: {}"), FPaths::EngineDir());
		PathUtilities::InitDefaultMountPoints(); // Initialize default mount points to enable path resolving.
		InitEngineThreadPool();

		FModuleManager::Get().LoadModule("RenderCore");
		DObjectInit();
	}

	auto FEngineLoop::Init() -> void
	{
#if DURIN_WITH_EDITOR
		GEngine = new DEditorEngine();
#else
		GEngine = new DGameEngine();
#endif

		ApplicationCoreInit();
		RHIInit();
		Mona::MonaInit();

		GEngine->Init();

		InitRenderingThread();
		DURIN_INFO(STR("Durin engine initialized."));
	}

	// Called from render thread
	static auto BeginFrameRenderThread(FRHICommandListImmediate& CommandList, uint64 LogicFrameCounter, uint64 RenderFrameCounter) -> void
	{
		check(IsInRenderingThread());
		GFrameCounterRenderThread = LogicFrameCounter;
		GRenderFrameCounterRenderThread = RenderFrameCounter;
		CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		GDynamicRHI->RHIBeginFrame();
	}

	// Called from render thread
	static auto EndFrameRenderThread(FRHICommandListImmediate& RHICmdList, uint64 LogicFrameCounter, uint64 RenderFrameCounter) -> void
	{
		check(IsInRenderingThread());
		check(GFrameCounterRenderThread == LogicFrameCounter);
		check(GRenderFrameCounterRenderThread == RenderFrameCounter);
		GDynamicRHI->RHIEndFrame_RenderThread(RHICmdList);
	}

	namespace
	{
		struct FScopedRenderFrameSubmission
		{
			explicit FScopedRenderFrameSubmission(bool& bInIsSubmitting)
				: bIsSubmitting(bInIsSubmitting)
			{
				check(!bIsSubmitting);
				bIsSubmitting = true;
			}

			~FScopedRenderFrameSubmission()
			{
				bIsSubmitting = false;
			}

			bool& bIsSubmitting;
		};
	}

	auto FEngineLoop::SubmitRenderFrame(
		const uint64 LogicFrameCounter,
		const uint64 RenderFrameCounter,
		const bool bRenderSceneViewports,
		const std::function<void()>& QueueUiRenderWork
	) -> bool
	{
		if (bIsSubmittingRenderFrame || GDynamicRHI == nullptr)
		{
			return false;
		}

		FScopedRenderFrameSubmission RenderFrameSubmission(bIsSubmittingRenderFrame);

		ENQUEUE_RENDER_COMMAND(BeginFrame)([LogicFrameCounter, RenderFrameCounter](FRHICommandListImmediate& CommandList) {
			BeginFrameRenderThread(CommandList, LogicFrameCounter, RenderFrameCounter);
		});

		if (bRenderSceneViewports && GEngine != nullptr)
		{
			GEngine->RedrawViewports();
		}

		if (QueueUiRenderWork)
		{
			QueueUiRenderWork();
		}

		ENQUEUE_RENDER_COMMAND(EndFrame)([LogicFrameCounter, RenderFrameCounter](FRHICommandListImmediate& RHICmdList) {
			EndFrameRenderThread(RHICmdList, LogicFrameCounter, RenderFrameCounter);
		});

		FFrameSync::Sync(FFrameSync::EFlushMode::EndFrame);
		return true;
	}

	auto FEngineLoop::Tick() -> void
	{
		const uint64 CurrentLogicFrameCounter = GFrameCounter;

		// Game logic.
		GEngine->Tick(0.0f, false);

		// Process application events, and paint UI.
		Mona::FMonaApplication::Get().Tick();

		if (GIsRequestingExit) return;

		const uint64 CurrentRenderFrameCounter = GRenderFrameCounter;
		Mona::NewFrame();
		if (SubmitRenderFrame(
			CurrentLogicFrameCounter,
			CurrentRenderFrameCounter,
			true,
			[]() {
				Mona::Render();
			}
		))
		{
			GFrameCounter++;
			GRenderFrameCounter++;
		}

		CalculateFPSTimings();
	}

	auto FEngineLoop::Exit() -> void
	{
		Mona::MonaShutdown();

		ShutdownEngineThreadPool(true);

		delete GEngine;
		GEngine = nullptr;

		FlushRenderingCommands();
		ShutdownRenderingThread();

		FModuleManager::Get().UnloadModulesAtShutdown();
		RHIExit();

		ApplicationCoreShutdown();
		DURIN_INFO(STR("Durin Engine exited."));
	}
} // namespace Durin
