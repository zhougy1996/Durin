#include "EngineLoop.h"

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
#include "Diagnostics/ProcessCrashContext.h"
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

#include "EngineFrame.h"
#include "RuntimeStorage.h"
#include "Windows/WindowsProcessCrashHandler.h"

#if DURIN_WITH_EDITOR
	#include "Editor/EditorEngine.h"
#else
	#include "Engine/GameEngine.h"
#endif


namespace Durin
{
	FEngineLoop::FEngineLoop(FLaunchDiagnosticsRequest DiagnosticsRequest)
		: Diagnostics(std::move(DiagnosticsRequest))
	{
	}

	auto FEngineLoop::PreInit(const FEngineStartupParams& Params) -> bool
	{
		if (State != EEngineLoopState::Uninitialized) return false;
		State = EEngineLoopState::PreInitializing;
		SetProcessCrashPhase(EProcessCrashPhase::PreInitialization);
		DURIN_PROFILE_CPU_ZONE_NAMED("Startup.PreInit");
		DURIN_PROFILE_THREAD("GameThread");
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		GIsWindowDisplaySuppressed = Params.bSuppressWindowDisplay;
		FPlatformMisc::EnableUserBinaryDirectoriesSearch();
		FPlatformMisc::AddRuntimeBinaryDirectory(FPaths::EngineThirdPartyRuntimeBinariesDir().c_str());

		FRuntimeStoragePreparationResult RuntimeStorage = PrepareRuntimeStorage();
		PublishWindowsProcessCrashRoot(FPaths::LaunchSavedDir());
		Diagnostics.AtPreInitialization();
		LoadAppConfig(RuntimeStorage.AppConfigPath.string());

		FNameInit(); // Initialize FName system.
		LoggerInit();
		bLoggerStarted = true;
		Diagnostics.AfterLoggerStarted();
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
#if DURIN_WITH_EDITOR
		if (HasCurrentProject()
			&& !AcquireProjectAuthoringOwnership(&ProjectError))
		{
			DURIN_ERROR("Editor project ownership failed: {}", ProjectError);
			return FailPreInitialization();
		}
		bProjectAuthoringOwnershipAcquired = HasCurrentProject();
#endif
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
			return FailPreInitialization();
		}
		if (!InitializeTaskScheduler())
		{
			DURIN_ERROR("Engine pre-initialization failed because the task scheduler could not start.");
			return FailPreInitialization();
		}
		bTaskSchedulerStarted = true;
		if (!InitializeGameThreadDeferredExecutor())
		{
			DURIN_ERROR("Engine pre-initialization failed because the GameThread deferred executor could not start.");
			return FailPreInitialization();
		}
		bGameThreadDeferredExecutorStarted = true;

		FModuleManager::Get().LoadModule("RenderCore");
		DObjectInit();
		InitializeEngineAssetServices();
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::PreInitComplete);
		State = EEngineLoopState::PreInitialized;
		return true;
	}

	auto FEngineLoop::FailPreInitialization() -> bool
	{
		if (bGameThreadDeferredExecutorStarted)
		{
			ShutdownTaskSystem(ETaskShutdownMode::Drain);
			bGameThreadDeferredExecutorStarted = false;
			bTaskSchedulerStarted = false;
		}
		else if (bTaskSchedulerStarted)
		{
			ShutdownTaskScheduler(false);
			bTaskSchedulerStarted = false;
		}
		if (bProjectAuthoringOwnershipAcquired)
		{
			ReleaseProjectAuthoringOwnership();
			bProjectAuthoringOwnershipAcquired = false;
		}
		State = EEngineLoopState::Exited;
		return false;
	}

	auto FEngineLoop::FailInitializationAfterRHI() -> bool
	{
		ShutdownEngineAssetServices();
		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		bGameThreadDeferredExecutorStarted = false;
		bTaskSchedulerStarted = false;
		RemoveFromRoot(GEngine);
		MarkObjectHierarchyAsGarbage(GEngine);
		GEngine = nullptr;
		Asset::ShutdownAssetManager();
		ReleaseClassDefaultObjects();
		ReleaseDStructDefaults();
		CollectGarbage();
		if (GRenderingThread) FlushRenderingCommands();
		CollectGarbage();
		FModuleManager::Get().UnloadModulesAtShutdown();
		ShutdownRenderingThread();
		RHIExit();
		ShutdownApplicationCore();
		if (bProjectAuthoringOwnershipAcquired)
		{
			ReleaseProjectAuthoringOwnership();
			bProjectAuthoringOwnershipAcquired = false;
		}
		State = EEngineLoopState::Exited;
		return false;
	}

	auto FEngineLoop::Init() -> bool
	{
		if (State != EEngineLoopState::PreInitialized) return false;
		State = EEngineLoopState::Initializing;
		SetProcessCrashPhase(EProcessCrashPhase::EngineInitialization);
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
				return FailInitializationAfterRHI();
			}
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::RHIReady);
		// Command admission must be running before Mona, the renderer, or editor
		// modules can publish their first render-thread work.
		InitRenderingThread();
		FModuleManager::Get().LoadModuleChecked("Mona");

		FEngineInitContext EngineInitContext;
		EngineInitContext.bHeadless = GIsWindowDisplaySuppressed;
		EngineInitContext.PumpStartupFrame = []() {
			constexpr double StartupWaitSeconds = 1.0 / 60.0;
			auto& Application = Mona::FMonaApplication::Get();
			Application.Tick();
			if (IsEngineExitRequested()) return false;
			if (GIsWindowDisplaySuppressed) return true;
			if (Application.AreAllWindowsMinimized())
			{
				Application.WaitForEvents(StartupWaitSeconds);
				return !IsEngineExitRequested();
			}
			RenderEngineStartupFrame();
			return !IsEngineExitRequested();
		};
		const FEngineInitializationResult EngineInitResult =
			GEngine->Init(EngineInitContext);
		if (!EngineInitResult)
		{
			bInitializationCancelled = EngineInitResult.Status
				== EEngineInitializationStatus::Cancelled;
			if (bInitializationCancelled)
				DURIN_INFO("{}", EngineInitResult.Message.empty()
					? "Engine initialization was cancelled." : EngineInitResult.Message);
			else
				DURIN_ERROR("Engine initialization failed: {}", EngineInitResult.Message);
			return FailInitializationAfterRHI();
		}
		LastTickTime = FTime::Seconds();

		DURIN_INFO(STR("Durin engine initialized."));
		SetProcessCrashPhase(EProcessCrashPhase::Running);
		Diagnostics.AfterEngineInitialized();
		State = EEngineLoopState::Running;
		return true;
	}

	auto FEngineLoop::Tick() -> void
	{
		check(State == EEngineLoopState::Running);
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
		Diagnostics.Tick();
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
		if (State == EEngineLoopState::Exited || State == EEngineLoopState::Uninitialized) return;
		if (State != EEngineLoopState::Running)
		{
			FailPreInitialization();
			return;
		}
		State = EEngineLoopState::ShuttingDown;
		SetProcessCrashPhase(EProcessCrashPhase::ConsumerDetachment);
		Diagnostics.BeginConsumerDetachment();

		FModuleManager::Get().ShutdownModule("Mona");

		Diagnostics.BeforeAssetServiceShutdown();
		SetProcessCrashPhase(EProcessCrashPhase::AssetServiceShutdown);
		ShutdownEngineAssetServices();
		SetProcessCrashPhase(EProcessCrashPhase::TaskSystemShutdown);
		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		Diagnostics.AfterTaskSystemShutdown();

		RemoveFromRoot(GEngine);
		MarkObjectHierarchyAsGarbage(GEngine);
		GEngine = nullptr;
		AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::EngineRootRetired);
		SetProcessCrashPhase(EProcessCrashPhase::AssetManagerShutdown);
		Asset::ShutdownAssetManager();
		ReleaseClassDefaultObjects();
		AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::ClassDefaultsReleased);
		ReleaseDStructDefaults();
		AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::StructDefaultsReleased);
		SetProcessCrashPhase(EProcessCrashPhase::ObjectCollection);
		AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::FirstObjectCollection);
		Diagnostics.AtObjectCollection();
		CollectGarbage();

		if (GRenderingThread)
		{
			FlushRenderingCommands();
			AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::RenderingCommandsFlushed);
		}
		AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::SecondObjectCollection);
		CollectGarbage();
		AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::DeferredDestroyAudit);
		CheckNoDeferredDestroyObjects("shutdown object destruction");
		SetProcessCrashPhase(EProcessCrashPhase::ModuleShutdown);
		FModuleManager::Get().UnloadModulesAtShutdown();
		AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::ModulesUnloaded);
		SetProcessCrashPhase(EProcessCrashPhase::RenderingShutdown);
		ShutdownRenderingThread();
		SetProcessCrashPhase(EProcessCrashPhase::RHIShutdown);
		RHIExit();

		SetProcessCrashPhase(EProcessCrashPhase::ApplicationShutdown);
		ShutdownApplicationCore();
		if (bProjectAuthoringOwnershipAcquired) ReleaseProjectAuthoringOwnership();
		bProjectAuthoringOwnershipAcquired = false;
		DURIN_INFO(STR("Durin Engine exited."));
		State = EEngineLoopState::Exited;
	}
} // namespace Durin
