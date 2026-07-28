#include "LaunchEngineLoop.h"

#include "Threading/QueuedThreadPool.h"
#include "Threading/RunnableThread.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "ApplicationCore.h"
#include "AssetSystem.h"
#include "RHI.h"
#include "Mona.h"
#include "Engine/Engine.h"

#include "RHICommandList.h"
#include "RenderResource.h"
#include "RenderingThread.h"
#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AppConfig.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/Time.h"
#include "Misc/Version.h"
#include "Profiling/Profiling.h"

#include "Shader/ShaderPaths.h"
#include "EngineGlobals.h"

#if DURIN_WITH_EDITOR
	#include "Editor/EditorEngine.h"
#else
	#include "Engine/GameEngine.h"
#endif


namespace Durin
{
	constexpr std::string_view AppConfigFileName = DURIN_RUNTIME_VARIANT ".yaml";

	FEngineLoop GEngineLoop;

	auto FEngineLoop::PreInit(std::span<const std::string_view> Arguments) -> void
	{
		DURIN_PROFILE_THREAD("GameThread");
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		GIsWindowDisplaySuppressed = std::ranges::find(Arguments, std::string_view("--hidden-window")) != Arguments.end();

		FPlatformMisc::EnableUserBinaryDirectoriesSearch();
		FPlatformMisc::AddRuntimeBinaryDirectory(FPaths::EngineThirdPartyRuntimeBinariesDir().c_str());

		LoadAppConfig(FPaths::LaunchDir() + std::string(AppConfigFileName));

		FNameInit(); // Initialize FName system.
		LoggerInit();
		DURIN_INFO(STR("Launching Durin Engine {}..."), GetEngineVersionString());
#if DURIN_WITH_TRACY
		if (const char* TracyPort = std::getenv("TRACY_PORT"))
			DURIN_WARN("{}", Profiling::FormatPortOverrideDiagnostic(TracyPort));
#endif
		DURIN_DEBUG(STR("Launch directory: {}"), FPaths::LaunchDir());
		DURIN_DEBUG(STR("Engine directory: {}"), FPaths::EngineDir());
		std::string ProjectError;
		if (!InitializeCurrentProject(Arguments, &ProjectError) && !ProjectError.empty()) DURIN_WARN("{}", ProjectError);
		DURIN_PROFILE_PROGRAM_IDENTITY(
			DURIN_RUNTIME_VARIANT,
			GetCurrentProject() ? std::string_view{GetCurrentProject()->Name} : std::string_view{},
			FPlatformProcess::CurrentProcessId()
		);
		if (!FPaths::ProjectFile().empty()) DURIN_DEBUG(STR("Project file: {}"), FPaths::ProjectFile());
		std::string MountError;
		checkf(
			PathUtilities::InitDefaultMountPoints(&MountError),
			"Failed to initialize mount registry: {}", MountError);
		InitEngineThreadPool();

		FModuleManager::Get().LoadModule("RenderCore");
		DObjectInit();
	}

	auto FEngineLoop::Init() -> void
	{
#if DURIN_WITH_EDITOR
		GEngine = NewObject<DEditorEngine>(nullptr, "EditorEngine");
#else
		GEngine = NewObject<DGameEngine>(nullptr, "GameEngine");
#endif
		AddToRoot(GEngine);

		InitializeApplicationCore();
		RHIInit();
		// Command admission must be running before Mona, the renderer, or editor
		// modules can publish their first render-thread work.
		InitRenderingThread();
		Mona::MonaInit();

		GEngine->Init();
		LastTickTime = FTime::Seconds();

		DURIN_INFO(STR("Durin engine initialized."));
	}

	// Called from render thread
	static auto BeginFrameRenderThread(FRHICommandListImmediate& CommandList, uint64 LogicFrameCounter, uint64 RenderFrameCounter) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("RenderFrame.Begin");
		check(IsInRenderingThread());
		GFrameCounterRenderThread = LogicFrameCounter;
		GRenderFrameCounterRenderThread = RenderFrameCounter;
		CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		GDynamicRHI->RHIBeginFrame();
	}

	// Called from render thread
	static auto EndFrameRenderThread(FRHICommandListImmediate& RHICmdList, uint64 LogicFrameCounter, uint64 RenderFrameCounter) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("RenderFrame.End");
		check(IsInRenderingThread());
		check(GFrameCounterRenderThread == LogicFrameCounter);
		check(GRenderFrameCounterRenderThread == RenderFrameCounter);
		GDynamicRHI->RHIEndFrame_RenderThread(RHICmdList);
	}

	static auto RenderFrame() -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.RenderFrame");
		if (GDynamicRHI == nullptr)
		{
			return;
		}

		const uint64 LogicFrameCounter = GFrameCounter;
		const uint64 RenderFrameCounter = GRenderFrameCounter;

		ENQUEUE_RENDER_COMMAND(BeginFrame)([LogicFrameCounter, RenderFrameCounter](FRHICommandListImmediate& CommandList) {
			BeginFrameRenderThread(CommandList, LogicFrameCounter, RenderFrameCounter);
		});

		if (GEngine != nullptr)
		{
			GEngine->RedrawViewports();
		}

		Mona::Render();

		ENQUEUE_RENDER_COMMAND(EndFrame)([LogicFrameCounter, RenderFrameCounter](FRHICommandListImmediate& RHICmdList) {
			EndFrameRenderThread(RHICmdList, LogicFrameCounter, RenderFrameCounter);
		});

		FFrameSync::Sync(FFrameSync::EFlushMode::EndFrame);
		GRenderFrameCounter++;
	}

	auto FEngineLoop::Tick() -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.Tick");
		constexpr double MinimizedTickIntervalSeconds = 1.0 / 20.0;

		// Game logic.
		const double CurrentTime = FTime::Seconds();
		const float DeltaSeconds = static_cast<float>(std::clamp(CurrentTime - LastTickTime, 0.0, 0.1));
		LastTickTime = CurrentTime;
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.GameLogic");
			GEngine->Tick(DeltaSeconds, false);
		}
		GFrameCounter++;

		// Process application events, and paint UI.
		auto& Application = Mona::FMonaApplication::Get();
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.Application");
			Application.Tick();
		}

		if (GIsRequestingExit) return;

		const bool bAllWindowsMinimized = Application.AreAllWindowsMinimized();
		if (!bAllWindowsMinimized)
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.Rendering");
			Mona::NewFrame();
			RenderFrame();
		}
		const uint64 ObjectsBeforeGC = GDObjectArray.GetNum();
		const uint64 PendingKillBeforeGC = GetGarbageObjectCount();
		const double GCStartTime = FTime::Seconds();
		EGarbageCollectionTrigger GCTrigger;
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.GarbageCollection");
			GCTrigger = TryCollectGarbage(GCStartTime);
		}
		if (GCTrigger != EGarbageCollectionTrigger::None)
		{
			const FGarbageCollectionStats& GCStats = GetLastGarbageCollectionStats();
			const char* TriggerName = GCTrigger == EGarbageCollectionTrigger::Interval ? "interval"
				: GCTrigger == EGarbageCollectionTrigger::PendingKillPressure ? "pending-kill pressure"
				: "object-growth pressure";
			DURIN_INFO_CATEGORY("GC", "Automatic GC ({}) completed in {:.3f} ms (mark {:.3f} ms, sweep {:.3f} ms): "
				"objects {} -> {}, marked {}, candidates {}, swept {}, deferred {}, pending kill {}, next interval {:.1f} s.", TriggerName,
				(FTime::Seconds() - GCStartTime) * 1000.0, GCStats.MarkMilliseconds, GCStats.SweepMilliseconds,
				ObjectsBeforeGC, GDObjectArray.GetNum(), GCStats.MarkedObjectCount, GCStats.CandidateObjectCount,
				GCStats.SweptObjectCount, GCStats.DeferredDestroyObjectCount, PendingKillBeforeGC,
				GetCurrentAutomaticGarbageCollectionIntervalSeconds());
		}

		CalculateFPSTimings();
		if (bAllWindowsMinimized)
		{
			// Present normally paces the loop. Once every window is minimized there is
			// no present, so wait for events while retaining a low-frequency engine tick.
			Application.WaitForEvents(MinimizedTickIntervalSeconds);
		}
		DURIN_PROFILE_FRAME_MARK();
	}

	auto FEngineLoop::Exit() -> void
	{
		Mona::MonaShutdown();

		ShutdownEngineThreadPool(true);

		RemoveFromRoot(GEngine);
		MarkObjectHierarchyAsGarbage(GEngine);
		GEngine = nullptr;
		Asset::ShutdownAssetManager();
		CollectGarbage();

		FlushRenderingCommands();
		FModuleManager::Get().UnloadModulesAtShutdown();
		FlushRenderingCommands();
		FinalizeRenderingThreadBeforeRHIExit();
		ShutdownRenderingThread();

		check(GetRenderCommandAdmissionState()
			== ERenderCommandAdmissionState::Stopped);
		check(GetNumPendingRenderCommands() == 0);
		check(GetNumInitializedRenderResources() == 0);
		check(GetNumPendingRenderResourceCleanup() == 0);
		check(FRHIResource::GetNumPendingDeletes() == 0);
		RHIExit();

		ShutdownApplicationCore();
		DURIN_INFO(STR("Durin Engine exited."));
	}
} // namespace Durin
