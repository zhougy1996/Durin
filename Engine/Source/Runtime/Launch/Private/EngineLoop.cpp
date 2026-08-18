#include "EngineLoop.h"

#include "Threading/Task.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "ApplicationCore.h"
#include "Application/ModalLoopTick.h"
#include "AssetLoad.h"
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
#include "EngineFramePhases.h"
#include "RuntimeStorage.h"
#include "ProcessCrashServices.h"

#if DURIN_WITH_EDITOR
	#include "Editor/EditorEngine.h"
#else
	#include "Engine/GameEngine.h"
#endif


namespace Durin
{
	namespace
	{
		FEngineLoop* GModalLoopFrameOwner = nullptr;
	}
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
		PublishProcessCrashRoot(FPaths::LaunchSavedDir());
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
			Exit();
			return false;
		}
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
			Exit();
			return false;
		}
		if (!InitializeTaskScheduler())
		{
			DURIN_ERROR("Engine pre-initialization failed because the task scheduler could not start.");
			Exit();
			return false;
		}
		bTaskSchedulerStarted = true;
		if (!InitializeGameThreadDeferredExecutor())
		{
			DURIN_ERROR("Engine pre-initialization failed because the GameThread deferred executor could not start.");
			Exit();
			return false;
		}
		bGameThreadDeferredExecutorStarted = true;

		FModuleManager::Get().LoadModule("RenderCore");
		DObjectInit();
		InitializeEngineAssetServices();
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::PreInitComplete);
		State = EEngineLoopState::PreInitialized;
		return true;
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

		if (!InitializeApplicationCore())
		{
			DURIN_ERROR("Engine initialization stopped because ApplicationCore could not start.");
			Exit();
			return false;
		}

		if (!FModuleManager::Get().LoadModule("Mona"))
		{
			DURIN_ERROR("Engine initialization stopped because Mona platform services could not start.");
			Exit();
			return false;
		}
		StartupWindow = std::make_shared<MWindow>();
#if DURIN_WITH_EDITOR
		StartupWindow->SetWindowDecorationMode(EWindowDecorationMode::CustomTitleBar);
		StartupWindow->SetTitle(GetCurrentProject()
			? std::format("Durin Editor - {}", GetCurrentProject()->Name)
			: "Durin Editor - Project Browser");
#else
		StartupWindow->SetTitle(GetCurrentProject()
			? GetCurrentProject()->Name : "DurinGame");
#endif
		StartupWindow->ReshapeWindow({100.0f, 100.0f}, {1280.0f, 720.0f});
		Mona::FMonaApplication::Get().AddWindow(StartupWindow, false);
		const std::shared_ptr<FGenericWindow> StartupNativeWindow =
			StartupWindow->GetNativeWindow();
		if (!StartupNativeWindow
			|| !StartupNativeWindow->GetOSNativeWindowHandle())
		{
			DURIN_ERROR("Engine initialization stopped because the primary native window could not be created.");
			Exit();
			return false;
		}
		const FRHIPresentationTarget PresentationTarget{
			.NativeWindowHandle =
				StartupNativeWindow->GetOSNativeWindowHandle()};
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.RHIInitialization");
			if (!RHIInit(FRHIInitializationContext::Presentation(
					PresentationTarget)))
			{
				DURIN_ERROR(
					"Engine initialization stopped because the dynamic RHI could not start.");
				Exit();
				return false;
			}
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::RHIReady);
		// Command admission must be running before Mona rendering or editor
		// modules can publish their first render-thread work.
		InitRenderingThread();
		if (!Mona::InitializeRendering(true))
		{
			DURIN_ERROR("Engine initialization stopped because Mona rendering services could not start.");
			Exit();
			return false;
		}

		FEngineInitContext EngineInitContext;
		EngineInitContext.StartupWindow = StartupWindow;
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
			Exit();
			return false;
		}
		LastTickTime = FTime::Seconds();

		DURIN_INFO(STR("Durin engine initialized."));
		SetProcessCrashPhase(EProcessCrashPhase::Running);
		Diagnostics.AfterEngineInitialized();
		State = EEngineLoopState::Running;
		GModalLoopFrameOwner = this;
		SetModalLoopTickCallback([]() {
			if (GModalLoopFrameOwner != nullptr)
			{
				GModalLoopFrameOwner->TickModalContinuation();
			}
		});
		return true;
	}

	auto FEngineLoop::Tick() -> void
	{
		check(State == EEngineLoopState::Running);
		DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.Tick");
		auto& Application = Mona::FMonaApplication::Get();
		RunInteractiveFramePhases(
			FrameState,
			[&Application]() {
				DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.PlatformInput");
				Application.PumpPlatformEvents();
			},
			[this]() { TickPostEventFrame(true); },
			[]() { return GIsRequestingExit; });
	}

	auto FEngineLoop::TickPostEventFrame(bool bAllowMinimizedWait) -> void
	{
		constexpr double MinimizedTickIntervalSeconds = 1.0 / 20.0;
		const double CurrentTime = FTime::Seconds();
		const float DeltaSeconds = static_cast<float>(std::clamp(CurrentTime - LastTickTime, 0.0, 0.1));
		LastTickTime = CurrentTime;
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.GameLogic");
			GEngine->Tick(DeltaSeconds, false);
		}
		Diagnostics.Tick();
		PumpGameThreadDeferredWork();
		GFrameCounter++;

		auto& Application = Mona::FMonaApplication::Get();
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.ApplicationUI");
			Application.TickUI();
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
		if (bAllWindowsMinimized && bAllowMinimizedWait)
		{
			// Present normally paces the loop. Once every window is minimized there is
			// no present, so wait for events while retaining a low-frequency engine tick.
			Application.WaitForEvents(MinimizedTickIntervalSeconds);
		}
		PublishTaskSchedulerProfilerPlots();
		DURIN_PROFILE_FRAME_MARK();
	}

	auto FEngineLoop::TickModalContinuation() -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.ModalContinuation");
		TryRunModalContinuationFrame(
			FrameState,
			State == EEngineLoopState::Running && !GIsRequestingExit,
			[this]() {
				TickPostEventFrame(false);
				// Keep resize and draw commands inside the current native callback.
				// Windows cannot advance the surface extent until this callback returns.
				FFrameSync::Sync(FFrameSync::EFlushMode::Threads);
			});
	}

	auto FEngineLoop::Exit() -> void
	{
		if (State == EEngineLoopState::Exited || State == EEngineLoopState::Uninitialized
			|| State == EEngineLoopState::ShuttingDown) return;
		const bool bWasRunning = State == EEngineLoopState::Running;
		SetModalLoopTickCallback(nullptr);
		GModalLoopFrameOwner = nullptr;
		FrameState = EInteractiveFrameState::ShuttingDown;
		State = EEngineLoopState::ShuttingDown;
		if (bWasRunning)
		{
			SetProcessCrashPhase(EProcessCrashPhase::ConsumerDetachment);
			Diagnostics.BeginConsumerDetachment();
		}

		if (FModuleManager::Get().IsModuleLoaded("Mona"))
		{
			const auto MonaShutdown = FModuleManager::Get().ShutdownModule("Mona");
			if (!MonaShutdown.Succeeded())
			{
				DURIN_ERROR(STR("Mona module shutdown failed: {}"), MonaShutdown.Message);
			}
		}

		if (bWasRunning)
		{
			Diagnostics.BeforeAssetServiceShutdown();
			SetProcessCrashPhase(EProcessCrashPhase::AssetServiceShutdown);
		}
		if (GEngine) GEngine->PrepareForShutdown();
		if (bGameThreadDeferredExecutorStarted)
		{
			SetProcessCrashPhase(EProcessCrashPhase::TaskSystemShutdown);
			ShutdownTaskSystem(ETaskShutdownMode::Drain);
			bGameThreadDeferredExecutorStarted = false;
			bTaskSchedulerStarted = false;
			if (bWasRunning) Diagnostics.AfterTaskSystemShutdown();
		}
		else if (bTaskSchedulerStarted)
		{
			ShutdownTaskScheduler(false);
			bTaskSchedulerStarted = false;
		}

		if (GEngine)
		{
			RemoveFromRoot(GEngine);
			MarkObjectHierarchyAsGarbage(GEngine);
			GEngine = nullptr;
		}
		StartupWindow.reset();
		if (IsDObjectInitialized())
		{
			if (bWasRunning)
			{
				AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::EngineRootRetired);
				SetProcessCrashPhase(EProcessCrashPhase::AssetManagerShutdown);
			}
			Asset::ShutdownAssetManager();
			ReleaseClassDefaultObjects();
			if (bWasRunning)
				AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::ClassDefaultsReleased);
			ReleaseDStructDefaults();
			if (bWasRunning)
			{
				AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::StructDefaultsReleased);
				SetProcessCrashPhase(EProcessCrashPhase::ObjectCollection);
				AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::FirstObjectCollection);
				Diagnostics.AtObjectCollection();
			}
			CollectGarbage();

			if (GRenderingThread)
			{
				FlushRenderingCommands();
				if (bWasRunning)
					AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::RenderingCommandsFlushed);
			}
			if (bWasRunning)
				AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::SecondObjectCollection);
			CollectGarbage();
			if (bWasRunning)
			{
				AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::DeferredDestroyAudit);
				CheckNoDeferredDestroyObjects("shutdown object destruction");
				SetProcessCrashPhase(EProcessCrashPhase::ModuleShutdown);
			}
			const std::array DeferredModules{FName("VulkanRHI")};
			FModuleManager::Get().UnloadModulesAtShutdown(DeferredModules);
		}
		if (bWasRunning) AddProcessCrashBreadcrumb(EProcessCrashBreadcrumbEvent::ModulesUnloaded);
		if (GRenderingThread)
		{
			if (bWasRunning) SetProcessCrashPhase(EProcessCrashPhase::RenderingShutdown);
			ShutdownRenderingThread();
		}
		if (GDynamicRHI)
		{
			if (bWasRunning) SetProcessCrashPhase(EProcessCrashPhase::RHIShutdown);
			RHIExit();
		}

		if (IsApplicationCoreInitialized())
		{
			if (bWasRunning) SetProcessCrashPhase(EProcessCrashPhase::ApplicationShutdown);
			ShutdownApplicationCore();
		}
#if DURIN_WITH_EDITOR
		ReleaseProjectAuthoringOwnership();
#endif
		if (bWasRunning) DURIN_INFO(STR("Durin Engine exited."));
		State = EEngineLoopState::Exited;
	}
} // namespace Durin
