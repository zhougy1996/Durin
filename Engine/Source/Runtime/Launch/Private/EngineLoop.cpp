#if DURIN_WITH_EDITOR
#include "DerivedDataCache/DerivedDataCacheDiagnostics.h"
#endif
#include "DObject/Package.h"
#include "Texture/Texture.h"
#include "EngineLoop.h"

#include "Threading/TaskRuntime.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "ApplicationCore.h"
#include "Application/ModalLoopTick.h"
#include "Asset/Asset.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include "Mona.h"
#include "Engine/Engine.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/CookedMeshLoadManager.h"
#include "Components/PrimitiveComponent.h"
#include "Shader/ShaderData.h"

#include "RenderingThread.h"
#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AppConfig.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/Project.h"
#include "Misc/Time.h"
#include "Misc/Version.h"
#include "Modules/ModuleManager.h"
#include "Profiling/Profiling.h"

#include "Engine/EngineGlobals.h"
#include "EngineFrame.h"
#include "EngineFramePhases.h"
#include "RuntimeStorage.h"
#include "ProcessCrashServices.h"
#include "Widgets/MWindow.h"
#include "Window/GenericWindow.h"

#if DURIN_WITH_EDITOR
	#include "Editor/EditorEngine.h"
	#include "MonaImGui.h"
#else
	#include "Engine/GameEngine.h"
#endif


namespace Durin
{
	namespace
	{
		FEngineLoop* GModalLoopFrameOwner = nullptr;
	}

	auto FEngineLoop::PreInit(const FEngineStartupParams& Params) -> bool
	{
		if (State != EEngineLoopState::Uninitialized) return false;
		State = EEngineLoopState::PreInitializing;
		DURIN_PROFILE_CPU_ZONE_NAMED("Startup.PreInit");
		DURIN_PROFILE_THREAD("GameThread");
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		FPlatformMisc::EnableUserBinaryDirectoriesSearch();
		FPlatformMisc::AddRuntimeBinaryDirectory(FPaths::EngineThirdPartyRuntimeBinariesDir().c_str());

		FRuntimeStoragePreparationResult RuntimeStorage = PrepareRuntimeStorage();
		PublishProcessCrashRoot(FPaths::LaunchSavedDir());
		LoadAppConfig(RuntimeStorage.AppConfigPath.string());

		FNameInit(); // Initialize FName system.
		LoggerInit();
		bLoggerStarted = true;
		for (const std::string& Warning : RuntimeStorage.Warnings) DURIN_WARN("{}", Warning);
		DURIN_INFO(STR("Engine starting: Durin {}"), GetEngineVersionString());
#if DURIN_WITH_TRACY
		if (const char* TracyPort = std::getenv("TRACY_PORT"))
			DURIN_WARN("{}", Profiling::FormatPortOverrideDiagnostic(TracyPort));
#endif
		DURIN_DEBUG(STR("Launch directory: {}"), FPaths::LaunchDir());
		DURIN_DEBUG(STR("Engine directory: {}"), FPaths::EngineDir());
		std::string ProjectError;
		const auto InitializeCurrentProjectResult = InitializeCurrentProject({
			.bOpenProjectBrowser = Params.bOpenProjectBrowser,
			.RequestedProjectFile = Params.ProjectFile.value_or(""),
			.bRecordInteractiveHistory = Params.bRecordInteractiveHistory});
		ProjectError = InitializeCurrentProjectResult ? std::string{} : InitializeCurrentProjectResult.error().ToString();
		ProjectAdmissionError = ProjectError;
		if (!InitializeCurrentProjectResult.has_value() && !ProjectError.empty()) DURIN_WARN("{}", ProjectError);
#if DURIN_WITH_EDITOR
		const auto AcquireProjectEditOwnershipResult = AcquireProjectEditOwnership();
		ProjectError = AcquireProjectEditOwnershipResult ? std::string{} : AcquireProjectEditOwnershipResult.error().ToString();
		if (HasCurrentProject()
			&& !AcquireProjectEditOwnershipResult.has_value())
		{
			DURIN_ERROR("Editor project ownership failed: {}", ProjectError);
			ProjectAdmissionError = ProjectError;
			ReleaseProjectEditOwnership();
			(void)InitializeCurrentProject({.bOpenProjectBrowser = true});
			(void)FPaths::SetProjectFile({});
		}
#endif
		DURIN_PROFILE_PROGRAM_IDENTITY(
			DURIN_RUNTIME_VARIANT,
			GetCurrentProject() ? std::string_view{GetCurrentProject()->Name} : std::string_view{},
			FPlatformProcess::CurrentProcessId()
		);
		if (!FPaths::ProjectFile().empty()) DURIN_DEBUG(STR("Project file: {}"), FPaths::ProjectFile());
		std::string MountError;
		std::filesystem::path CookedMountRoot;
#if !DURIN_WITH_EDITOR
        CookedMountRoot=std::filesystem::path(FPaths::LaunchDir()).lexically_normal();
#endif
        if (!FMountPaths::InitDefaultMountPoints(&MountError,true,CookedMountRoot))
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
			return false;
		}
		bGameThreadDeferredExecutorStarted = true;

		if (!FModuleManager::Get().LoadModule("RenderCore"))
		{
			DURIN_ERROR("Engine pre-initialization failed because RenderCore could not start.");
			return false;
		}
#if DURIN_WITH_EDITOR
		if (!FModuleManager::Get().LoadModule("TextureCompressor")
			|| !FModuleManager::Get().LoadModule("MeshBuilder"))
		{
			DURIN_ERROR("Engine pre-initialization failed because asset producer modules could not start.");
			return false;
		}
		if (!FModuleManager::Get().LoadModule("ShaderBuild"))
		{
			DURIN_ERROR("Engine pre-initialization failed because ShaderBuild could not start.");
			return false;
		}
#else
		const auto Backend = ResolveRHIBackend(std::getenv("DURIN_RHI_BACKEND"));
		if (!Backend)
		{
			DURIN_ERROR("Engine pre-initialization failed because the RHI backend selection is invalid.");
			return false;
		}
		const auto ShaderPlatform = *Backend == ERHIBackend::Metal
			? EShaderTargetPlatform::MacOS : EShaderTargetPlatform::Win64;
		FShaderOperationResult ShaderDataError;
		if (!(ShaderDataError = InitializeShaderData(FShaderDataConfiguration::Cooked(
				std::filesystem::path(FPaths::LaunchDir()).lexically_normal(),
				ShaderPlatform))))
		{
			DURIN_ERROR("Engine pre-initialization failed because Cooked Shader data could not start: {}", std::string(ShaderDataError.error()));
			return false;
		}
#endif
		if (!InitializeAssetCompilingManager())
		{
			DURIN_ERROR("Engine pre-initialization failed because the asset compiling manager could not start.");
			return false;
		}
		DObjectInit();
#if !DURIN_WITH_EDITOR
        // Establish the cooked domain before any Engine or project asset loads.
        FAssetRuntimeConfiguration AssetConfiguration=FAssetRuntimeConfiguration::Authored();
        const auto ConfigurationResult=FAssetRuntimeConfiguration::Cooked(
            std::filesystem::path(FPaths::LaunchDir()).lexically_normal(),AssetConfiguration);
        if (!ConfigurationResult)
        {
            DURIN_ERROR("Cooked asset configuration failed: {}",ConfigurationResult.Message);
            return false;
        }
        ShutdownAssetManager();
        const auto AssetResult=InitializeAssetManager(std::move(AssetConfiguration));
        if (!AssetResult)
        {
            DURIN_ERROR("Cooked asset initialization failed: {}",AssetResult.Message);
            return false;
        }
#endif
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::PreInitComplete);
		State = EEngineLoopState::PreInitialized;
		return true;
	}

	auto FEngineLoop::Init() -> bool
	{
		if (State != EEngineLoopState::PreInitialized) return false;
		State = EEngineLoopState::Initializing;
#if DURIN_WITH_EDITOR
		GEngine = NewObject<DEditorEngine>(nullptr, "EditorEngine");
#else
		GEngine = NewObject<DGameEngine>(nullptr, "GameEngine");
#endif
		AddToRoot(GEngine);
#if DURIN_WITH_EDITOR
		if (!static_cast<DEditorEngine*>(GEngine)->ActivateEditor()) return false;
#endif

		if (!InitializeApplicationCore())
		{
			DURIN_ERROR("Engine initialization stopped because ApplicationCore could not start.");
			return false;
		}

		if (!Mona::InitializeApplication())
		{
			DURIN_ERROR("Engine initialization stopped because Mona platform services could not start.");
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
			return false;
		}
		const FRHIPresentationTarget PresentationTarget{
			.PlatformTarget = StartupNativeWindow->GetPresentationTarget()};
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.RHIInitialization");
			if (!RHIInit(FRHIInitializationContext::Presentation(
					PresentationTarget)))
			{
				DURIN_ERROR(
					"Engine initialization stopped because the dynamic RHI could not start.");
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
			return false;
		}
#if DURIN_WITH_EDITOR
		if (!MonaImGui::Initialize())
		{
			DURIN_ERROR("Engine initialization stopped because the MonaImGui editor backend could not start.");
			return false;
		}
#endif

		FEngineInitContext EngineInitContext;
		EngineInitContext.StartupWindow = StartupWindow;
		EngineInitContext.ProjectAdmissionError = ProjectAdmissionError;
		if (!GEngine->Init(EngineInitContext))
		{
			bInitializationCancelled = IsEngineExitRequested();
			if (bInitializationCancelled)
				DURIN_INFO("Engine initialization was cancelled.");
			return false;
		}
		LastTickTime = FTime::Seconds();

		DURIN_INFO(STR("Engine initialized"));
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

	auto FEngineLoop::GetStartupState() const -> EHostStartupState
	{
		return GEngine ? GEngine->GetStartupState() : EHostStartupState::Failed;
	}

	auto FEngineLoop::GetStartupError() const -> std::string
	{
		return GEngine ? GEngine->GetStartupError() : "The engine host is unavailable.";
	}

	auto FEngineLoop::GetHostFrameState() const -> FHostFrameState
	{
		auto& Application = Mona::FMonaApplication::Get();
		return {
			.bStartupReady = GetStartupState() == EHostStartupState::Ready,
			.bEditor = DURIN_WITH_EDITOR != 0,
			.bMinimized = Application.AreAllWindowsMinimized(),
			.bFocused = std::ranges::any_of(Application.GetWindows(), [](const auto& Window) {
				return Window && Window->GetNativeWindow() && Window->GetNativeWindow()->IsFocused();
			}),
			.FrameRateLimit = GEngine ? GEngine->GetFrameRateLimit() : 0,
			.BackgroundFrameRateLimit = GEngine ? GEngine->GetBackgroundFrameRateLimit() : 30,
			.bProfilerConnected = Profiling::GetConnectionState().bConnected,
		};
	}

	auto FEngineLoop::TickPostEventFrame(bool bAllowMinimizedWait, bool bAdvanceHost) -> void
	{
		const double CurrentTime = FTime::Seconds();
		auto& Application = Mona::FMonaApplication::Get();
		const double WaitTimeout = GetHostFrameWaitTimeout(GetHostFrameState(), LastTickTime, CurrentTime);
		if (WaitTimeout > 0.0)
		{
			if (bAllowMinimizedWait) Application.WaitForEvents(WaitTimeout);
			return;
		}
		const float DeltaSeconds = static_cast<float>(std::clamp(CurrentTime - LastTickTime, 0.0, 0.1));
		LastTickTime = CurrentTime;
		if (GetStartupState() == EHostStartupState::Pending || GEngine->IsPreparingMapChange())
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
			const auto Deferred = PumpGameThreadDeferredWork({.MaxCallbacks = 63, .MaxSeconds = 0.001});
			PackageSavePrivate::PollAsyncSaves();
			const double Remaining = std::chrono::duration<double, std::milli>(Deadline - std::chrono::steady_clock::now()).count();
			if (Remaining > 0) ProcessAsyncLoading(Remaining, 1);
			if (std::chrono::steady_clock::now() < Deadline)
				FAssetCompilingManager::Get().ProcessAsyncTasks({64 - Deferred.ExecutedCallbacks - 1, Deadline});
		}
		else
		{
			PumpGameThreadDeferredWork();
			PackageSavePrivate::PollAsyncSaves();
			ProcessAsyncLoading();
			FAssetCompilingManager::Get().ProcessAsyncTasks(true);
		}
		if (bAdvanceHost && !GIsRequestingExit)
		{
			GEngine->TickMapChange();
			GEngine->AdvanceStartup();
		}
		if (GIsRequestingExit) return;
		const double EngineTickStarted = FTime::Seconds();
		if (bAdvanceHost && GetStartupState() == EHostStartupState::Ready)
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("EngineLoop.GameLogic");
			GEngine->Tick(DeltaSeconds, false);
			++CompletedRunningTicks;
		}
		else
		{
			PumpCookedMeshLoadManager();
			DPrimitiveComponent::ProcessPSOPrecacheRequests();
		}
		const float EngineTickMilliseconds = static_cast<float>(
			(FTime::Seconds() - EngineTickStarted) * 1000.0);
		GFrameCounter++;

		RecordEngineFrameTickTiming(EngineTickMilliseconds);
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
		const double RemainingWait = GetHostFrameWaitTimeout(GetHostFrameState(), LastTickTime, FTime::Seconds());
		if (RemainingWait > 0.0 && bAllowMinimizedWait)
		{
			Application.WaitForEvents(RemainingWait);
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
				TickPostEventFrame(false, false);
				// Keep resize and draw commands inside the current native callback.
				// Windows cannot advance the surface extent until this callback returns.
				FFrameSync::Sync(FFrameSync::EFlushMode::Threads);
			});
	}

	auto FEngineLoop::Exit() -> void
	{
		if (State == EEngineLoopState::Exited || State == EEngineLoopState::Uninitialized
			|| State == EEngineLoopState::ShuttingDown) return;
		SetModalLoopTickCallback(nullptr);
		GModalLoopFrameOwner = nullptr;
		FrameState = EInteractiveFrameState::ShuttingDown;
		State = EEngineLoopState::ShuttingDown;
		// Stop provider tasks before consumer modules begin teardown.
		ShutdownAssetCompilingManager();

		PackageSavePrivate::SetAsyncSaveAdmission(false);
		(void)DPackage::DrainAsyncSaves();
		if (GEngine) GEngine->PrepareForShutdown();
		CancelAsyncLoading();

#if DURIN_WITH_EDITOR
		MonaImGui::Shutdown();
#endif

		Mona::Shutdown();

		ShutdownCookedMeshLoadManager();

		if (GEngine)
		{
			RemoveFromRoot(GEngine);
			MarkObjectHierarchyAsGarbage(GEngine);
			GEngine = nullptr;
		}
		StartupWindow.reset();
		if (IsDObjectInitialized())
		{
			ShutdownAssetManager();
			ReleaseClassDefaultObjects();
			ReleaseDStructDefaults();
			CollectGarbage();

			if (GRenderingThread)
			{
				FlushRenderingCommands();
			}
			CollectGarbage();
			CheckNoDeferredDestroyObjects("shutdown object destruction");
			FModuleManager::Get().ShutdownModulesAtExit();
		}
#if DURIN_WITH_EDITOR
		DerivedData::LogCacheDiagnostics();
#endif

		if (GRenderingThread)
		{
			ShutdownRenderingThread();
		}
		if (GDynamicRHI)
		{
			RHIExit();
		}
		// Producers have retired through host/object/module shutdown. Render fences
		// still admit task events until the final render and RHI drains complete.
		ShutdownTaskSystem(bGameThreadDeferredExecutorStarted
			? ETaskShutdownMode::Drain : ETaskShutdownMode::Cancel);
		bGameThreadDeferredExecutorStarted = false;

		ShutdownApplicationCore();
#if DURIN_WITH_EDITOR
		ReleaseProjectEditOwnership();
#endif
		if (bLoggerStarted) DURIN_INFO(STR("Engine exited"));
		State = EEngineLoopState::Exited;
	}
} // namespace Durin
