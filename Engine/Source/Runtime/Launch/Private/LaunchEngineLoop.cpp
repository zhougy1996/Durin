#include "LaunchEngineLoop.h"

#include "Threading/Task.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "ApplicationCore.h"
#include "AssetSystem.h"
#include "RHI.h"
#include "Mona.h"
#include "Engine/Engine.h"

#include "RenderingThread.h"
#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AppConfig.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/Time.h"
#include "Misc/Version.h"
#include "Modules/ModuleManager.h"
#include "Profiling/Profiling.h"

#include "Shader/ShaderPaths.h"
#include "EngineGlobals.h"
#include "EngineAssetServices.h"

#include "LaunchFrame.h"
#include "LaunchGameplayValidation.h"
#include "LaunchRuntimeStorage.h"
#include "LaunchTaskSchedulerValidation.h"

#if DURIN_WITH_EDITOR
	#include "Editor/EditorEngine.h"
#else
	#include "Engine/GameEngine.h"
#endif


namespace Durin
{
	FEngineLoop GEngineLoop;

	auto FEngineLoop::PreInit(const FEngineStartupParams& Params) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Startup.PreInit");
		DURIN_PROFILE_THREAD("GameThread");
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		GIsWindowDisplaySuppressed = Params.bSuppressWindowDisplay;
		bRunTaskSchedulerLifecycleSmoke =
			Params.bRunTaskSchedulerLifecycleSmoke;
		bRunEngineAssetServiceLifecycleSmoke =
			Params.bRunEngineAssetServiceLifecycleSmoke;
		bRunEditorPIELifecycleSmoke = Params.bRunEditorPIELifecycleSmoke;
		bRunNativeGameplayLifecycleSmoke = Params.bRunNativeGameplayLifecycleSmoke;

		FPlatformMisc::EnableUserBinaryDirectoriesSearch();
		FPlatformMisc::AddRuntimeBinaryDirectory(FPaths::EngineThirdPartyRuntimeBinariesDir().c_str());

		FLaunchRuntimeStorageResult RuntimeStorage = PrepareLaunchRuntimeStorage();
		LoadAppConfig(RuntimeStorage.AppConfigPath.string());

		FNameInit(); // Initialize FName system.
		LoggerInit();
		for (const std::string& Warning : RuntimeStorage.Warnings) DURIN_WARN("{}", Warning);
		DURIN_INFO(STR("Launching Durin Engine {}..."), GetEngineVersionString());
#if DURIN_WITH_TRACY
		if (const char* TracyPort = std::getenv("TRACY_PORT"))
			DURIN_WARN("{}", Profiling::FormatPortOverrideDiagnostic(TracyPort));
#endif
		DURIN_DEBUG(STR("Launch directory: {}"), FPaths::LaunchDir());
		DURIN_DEBUG(STR("Engine directory: {}"), FPaths::EngineDir());
		std::string ProjectError;
		if (!InitializeCurrentProject(Params.Project, &ProjectError) && !ProjectError.empty()) DURIN_WARN("{}", ProjectError);
		DURIN_PROFILE_PROGRAM_IDENTITY(
			DURIN_RUNTIME_VARIANT,
			GetCurrentProject() ? std::string_view{GetCurrentProject()->Name} : std::string_view{},
			FPlatformProcess::CurrentProcessId()
		);
		if (!FPaths::ProjectFile().empty()) DURIN_DEBUG(STR("Project file: {}"), FPaths::ProjectFile());
		std::string MountError;
		if (!PathUtilities::InitDefaultMountPoints(&MountError))
		{
			DURIN_ERROR("Failed to initialize mount registry: {}", MountError);
			return false;
		}
		if (!InitializeTaskScheduler())
		{
			DURIN_ERROR("Engine pre-initialization failed because the task scheduler could not start.");
			return false;
		}
		if (!InitializeGameThreadDeferredExecutor())
		{
			DURIN_ERROR("Engine pre-initialization failed because the GameThread deferred executor could not start.");
			ShutdownTaskScheduler(false);
			return false;
		}

		FModuleManager::Get().LoadModule("RenderCore");
		DObjectInit();
		InitializeEngineAssetServices();
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::PreInitComplete);
		return true;
	}

	auto FEngineLoop::Init() -> bool
	{
#if DURIN_WITH_EDITOR
		GEngine = NewObject<DEditorEngine>(nullptr, "EditorEngine");
#else
		GEngine = NewObject<DGameEngine>(nullptr, "GameEngine");
#endif
		AddToRoot(GEngine);

		InitializeApplicationCore();
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.RHIInitialization");
			if (!RHIInit())
			{
				DURIN_ERROR(
					"Engine initialization stopped because the dynamic RHI could not start.");
				ShutdownEngineAssetServices();
				ShutdownTaskSystem(ETaskShutdownMode::Drain);
				RemoveFromRoot(GEngine);
				MarkObjectHierarchyAsGarbage(GEngine);
				GEngine = nullptr;
				ReleaseClassDefaultObjects();
				ReleaseDStructDefaults();
				CollectGarbage();
				FModuleManager::Get().UnloadModulesAtShutdown();
				ShutdownApplicationCore();
				return false;
			}
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::RHIReady);
		// Command admission must be running before Mona, the renderer, or editor
		// modules can publish their first render-thread work.
		InitRenderingThread();
		FModuleManager::Get().LoadModuleChecked("Mona");

		GEngine->Init();
		if (bRunEngineAssetServiceLifecycleSmoke)
			BeginEngineAssetServiceLifecycleSmoke();
		LastTickTime = FTime::Seconds();

		DURIN_INFO(STR("Durin engine initialized."));
		return true;
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
#if DURIN_WITH_EDITOR
		if (bRunEditorPIELifecycleSmoke
			&& !bEditorPIELifecycleSmokeCompleted
			&& GEditor)
		{
			bEditorPIELifecycleSmokeCompleted = TryRunEditorPIELifecycleSmoke();
		}
#endif
		if (bRunNativeGameplayLifecycleSmoke
			&& !bNativeGameplayLifecycleSmokeCompleted)
		{
			RunNativeGameplayLifecycleSmoke();
			bNativeGameplayLifecycleSmokeCompleted = true;
		}
		PumpGameThreadDeferredWork();
		PumpEngineAssetServiceCompletions();
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
			RenderEngineFrame();
		}
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.GarbageCollection");
			TryCollectGarbage(FTime::Seconds());
		}

		CalculateFPSTimings();
		if (bAllWindowsMinimized)
		{
			// Present normally paces the loop. Once every window is minimized there is
			// no present, so wait for events while retaining a low-frequency engine tick.
			Application.WaitForEvents(MinimizedTickIntervalSeconds);
		}
		PublishTaskSchedulerProfilerPlots();
		DURIN_PROFILE_FRAME_MARK();
	}

	auto FEngineLoop::Exit() -> void
	{
#if DURIN_WITH_EDITOR
		checkf(!bRunEditorPIELifecycleSmoke || bEditorPIELifecycleSmokeCompleted,
			"Editor PIE lifecycle smoke never observed an active source Level.");
#endif
		checkf(!bRunNativeGameplayLifecycleSmoke
			|| bNativeGameplayLifecycleSmokeCompleted,
			"Native gameplay lifecycle smoke did not execute.");
		std::shared_ptr<FLaunchTaskSchedulerValidationState> TaskSchedulerValidation;
		if (bRunTaskSchedulerLifecycleSmoke)
		{
			TaskSchedulerValidation = BeginLaunchTaskSchedulerValidation();
		}

		FModuleManager::Get().ShutdownModule("Mona");

		if (bRunEngineAssetServiceLifecycleSmoke)
			ValidateEngineAssetServiceLifecycleSmoke();
		ShutdownEngineAssetServices();
		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		if (TaskSchedulerValidation)
		{
			ValidateLaunchTaskSchedulerShutdown(TaskSchedulerValidation);
		}

		RemoveFromRoot(GEngine);
		MarkObjectHierarchyAsGarbage(GEngine);
		GEngine = nullptr;
		Asset::ShutdownAssetManager();
		ReleaseClassDefaultObjects();
		ReleaseDStructDefaults();
		CollectGarbage();

		if (GRenderingThread) FlushRenderingCommands();
		CollectGarbage();
		CheckNoDeferredDestroyObjects("shutdown object destruction");
		FModuleManager::Get().UnloadModulesAtShutdown();
		ShutdownRenderingThread();
		RHIExit();

		ShutdownApplicationCore();
		DURIN_INFO(STR("Durin Engine exited."));
	}
} // namespace Durin
