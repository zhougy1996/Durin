#include "LaunchEngineLoop.h"

#include "Threading/RunnableThread.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "ApplicationCore.h"
#include "AssetSystem.h"
#include "RHI.h"
#include "Mona.h"
#include "Engine/Engine.h"

#include "RHICommandList.h"
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

#if DURIN_WITH_EDITOR
	#include "Editor/EditorEngine.h"
#else
	#include "Engine/GameEngine.h"
#endif


namespace Durin
{
	constexpr std::string_view AppConfigFileName = DURIN_RUNTIME_VARIANT ".yaml";

	namespace
	{
		std::vector<std::string> RuntimeStorageMigrationWarnings;

		auto MigrateLegacyRuntimeFile(std::string_view FileName) -> void
		{
			const std::filesystem::path LegacyPath = std::filesystem::path(FPaths::LaunchDir()) / FileName;
			const std::filesystem::path SavedPath = std::filesystem::path(FPaths::LaunchConfigsDir()) / FileName;
			if (!std::filesystem::exists(LegacyPath) || std::filesystem::exists(SavedPath)) return;

			std::error_code Error;
			std::filesystem::rename(LegacyPath, SavedPath, Error);
			if (!Error) return;

			Error.clear();
			std::filesystem::copy_file(LegacyPath, SavedPath, std::filesystem::copy_options::none, Error);
			if (!Error)
			{
				std::error_code RemoveError;
				std::filesystem::remove(LegacyPath, RemoveError);
				return;
			}
			RuntimeStorageMigrationWarnings.push_back(
				std::format("Could not migrate legacy runtime file '{}' to '{}': {}", LegacyPath.string(), SavedPath.string(), Error.message()));
		}

		auto PrepareRuntimeStorage() -> void
		{
			std::error_code Error;
			std::filesystem::create_directories(FPaths::LaunchSavedDir(), Error);
			if (Error)
			{
				RuntimeStorageMigrationWarnings.push_back(
					std::format("Could not create runtime saved directory '{}': {}", FPaths::LaunchSavedDir(), Error.message()));
				return;
			}

			const std::filesystem::path LegacyLogs = std::filesystem::path(FPaths::LaunchDir()) / "Logs";
			const std::filesystem::path SavedLogs = FPaths::LaunchLogsDir();
			if (std::filesystem::exists(LegacyLogs) && !std::filesystem::exists(SavedLogs))
			{
				std::filesystem::rename(LegacyLogs, SavedLogs, Error);
				if (Error)
				{
					RuntimeStorageMigrationWarnings.push_back(
						std::format("Could not migrate legacy log directory '{}' to '{}': {}", LegacyLogs.string(), SavedLogs.string(), Error.message()));
					Error.clear();
				}
			}

			std::filesystem::create_directories(FPaths::LaunchConfigsDir(), Error);
			if (Error)
			{
				RuntimeStorageMigrationWarnings.push_back(
					std::format("Could not create runtime config directory '{}': {}", FPaths::LaunchConfigsDir(), Error.message()));
				return;
			}
			std::filesystem::create_directories(FPaths::LaunchLogsDir(), Error);
			if (Error)
			{
				RuntimeStorageMigrationWarnings.push_back(
					std::format("Could not create runtime log directory '{}': {}", FPaths::LaunchLogsDir(), Error.message()));
			}

			MigrateLegacyRuntimeFile(AppConfigFileName);
			MigrateLegacyRuntimeFile("imgui.ini");
			MigrateLegacyRuntimeFile("EditorHostSettings.yaml");
			MigrateLegacyRuntimeFile("LevelEditorSession.yaml");
			MigrateLegacyRuntimeFile("ProjectHistory.yaml");
		}

		struct FEngineTaskSchedulerLifecycleSmoke
		{
			auto Begin() -> void
			{
				ShortTask = LaunchTask("EngineSmoke.Short", []() {});
				std::array<FTaskHandle, 1> ShortPrerequisites{ShortTask};
				FTaskLaunchOptions DependentOptions;
				DependentOptions.Prerequisites = ShortPrerequisites;
				DependentTask = LaunchTask(
					"EngineSmoke.Dependent", []() {}, DependentOptions);

				FailedTask = LaunchTask("EngineSmoke.Failure", []() {
					throw std::runtime_error("intentional engine lifecycle smoke failure");
				});
				std::array<FTaskHandle, 1> FailedPrerequisites{FailedTask};
				FTaskLaunchOptions FailedDependentOptions;
				FailedDependentOptions.Prerequisites = FailedPrerequisites;
				FailedDependentTask = LaunchTask(
					"EngineSmoke.FailureDependent", []() {},
					FailedDependentOptions);

				CancelableTask = LaunchCancelableTask(
					"EngineSmoke.Canceled",
					[](const FTaskCancellationToken& Token) {
						while (!Token.IsCancellationRequested())
						{
							std::this_thread::yield();
						}
					});
				const bool bCancellationRequested = CancelTask(CancelableTask);
				checkf(bCancellationRequested,
					"Engine scheduler lifecycle smoke could not cancel its task.");

				ParallelTask = LaunchTask("EngineSmoke.ParallelFor", [this]() {
					constexpr uint64 Num = 65'536;
					std::vector<uint64> Output(Num);
					FParallelForOptions Options;
					Options.MinBatchSize = 256;
					ParallelResult = ParallelFor(
						"EngineSmoke.ParallelWork", Num,
						[&Output](uint64 Index) {
							uint64 Value = Index + 0x9e3779b97f4a7c15ull;
							for (uint32 Round = 0; Round < 64; ++Round)
							{
								Value ^= Value >> 12;
								Value ^= Value << 25;
								Value ^= Value >> 27;
								Value *= 0x2545f4914f6cdd1dull;
							}
							Output[Index] = Value;
						}, Options);
					ParallelChecksum = Output[Num / 2];
				});

				WaiterTask = LaunchTask("EngineSmoke.Waiter", [this]() {
					WaitedState = WaitTask(DependentTask);
				});

				checkf(ShortTask.IsValid() && DependentTask.IsValid()
					&& FailedTask.IsValid()
					&& FailedDependentTask.IsValid() && CancelableTask.IsValid()
					&& ParallelTask.IsValid() && WaiterTask.IsValid(),
					"Engine scheduler lifecycle smoke could not launch its workload.");
				const std::array<FTaskHandle, 7> QualificationTasks{
					ShortTask, DependentTask, FailedTask, FailedDependentTask,
					CancelableTask, ParallelTask, WaiterTask};
				WaitAll(QualificationTasks);

				GameThreadSource = LaunchTask("EngineSmoke.GameThreadSource", []() {});
				FTaskContinuationOptions GameThreadOptions;
				GameThreadOptions.Target = ETaskTarget::GameThreadDeferred;
				GameThreadOptions.EstimatedPayloadBytes = 64;
				GameThreadDeferred = Then(
					GameThreadSource,
					"EngineSmoke.GameThreadDeferred",
					[this]() {
						bGameThreadDeferredRan = IsInGameThread();
					},
					GameThreadOptions
				);
				checkf(GameThreadSource.IsValid() && GameThreadDeferred.IsValid(),
					"Engine scheduler lifecycle smoke could not launch its deferred chain.");

				AdmissionProbe = LaunchTask("EngineSmoke.AdmissionProbe", [this]() {
					AdmissionProbeStarted.Trigger();
					while (IsTaskSchedulerRunning())
					{
						std::this_thread::yield();
					}
					bAdmissionRejected = !LaunchTask(
						"EngineSmoke.RejectedAfterClose", []() {}).IsValid();
				});
				checkf(AdmissionProbe.IsValid(),
					"Engine scheduler lifecycle smoke could not launch its admission probe.");
				const bool bAdmissionProbeStarted = AdmissionProbeStarted.WaitFor(1.0);
				checkf(bAdmissionProbeStarted,
					"Engine scheduler lifecycle smoke admission probe did not start.");
				SlowTask = LaunchTask("EngineSmoke.Long", []() {
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
				});
				checkf(SlowTask.IsValid(),
					"Engine scheduler lifecycle smoke could not launch its long task.");
				const FTaskSchedulerDiagnostics Diagnostics =
					GetTaskSchedulerDiagnostics();
				checkf(Diagnostics.bRunning
					&& Diagnostics.NonterminalTaskCount > 0,
					"Engine scheduler lifecycle smoke found no active workload before exit.");
			}

			auto ValidateAfterShutdown() const -> void
			{
				checkf(!IsTaskSchedulerRunning(),
					"Engine scheduler lifecycle smoke left the scheduler running.");
				checkf(bAdmissionRejected,
					"Engine scheduler lifecycle smoke admitted work after close.");
				checkf(AdmissionProbe.GetState() == ETaskState::Succeeded
					&& SlowTask.GetState() == ETaskState::Succeeded
					&& ShortTask.GetState() == ETaskState::Succeeded
					&& DependentTask.GetState() == ETaskState::Succeeded
					&& WaiterTask.GetState() == ETaskState::Succeeded
					&& GameThreadSource.GetState() == ETaskState::Succeeded
					&& GameThreadDeferred.GetState() == ETaskState::Succeeded
					&& bGameThreadDeferredRan,
					"Engine scheduler lifecycle smoke did not drain accepted work.");
				checkf(FailedTask.GetState() == ETaskState::Failed
					&& FailedDependentTask.GetState() == ETaskState::Canceled
					&& CancelableTask.GetState() == ETaskState::Canceled,
					"Engine scheduler lifecycle smoke produced incorrect failure or cancellation propagation.");
				checkf(WaitedState == ETaskState::Succeeded,
					"Engine scheduler lifecycle smoke waiter observed the wrong state.");
				checkf(ParallelTask.GetState() == ETaskState::Succeeded
					&& ParallelResult.State == ETaskState::Succeeded
					&& ParallelResult.ChunkCount > 1 && ParallelChecksum != 0,
					"Engine scheduler lifecycle smoke parallel workload failed.");

				const FTaskSchedulerDiagnostics Diagnostics =
					GetTaskSchedulerDiagnostics();
				const FGameThreadDeferredWorkQueueDiagnostics DeferredDiagnostics =
					GetGameThreadDeferredWorkQueueDiagnostics();
				checkf(!Diagnostics.bRunning
					&& Diagnostics.NonterminalTaskCount == 0
					&& Diagnostics.ActiveWorkerCount == 0
					&& Diagnostics.FailedTaskCount >= 1
					&& Diagnostics.CanceledTaskCount >= 2
					&& Diagnostics.RejectedTaskCount >= 1
					&& Diagnostics.LongWaitCount >= 1
					&& Diagnostics.RetainedTerminalHandleCount >= 9,
					"Engine scheduler lifecycle smoke diagnostics were incomplete.");
				checkf(!DeferredDiagnostics.bInstalled
					&& DeferredDiagnostics.PumpedCallbackCount >= 1,
					"Engine scheduler lifecycle smoke did not drain the GameThread executor.");
				DURIN_INFO(
					"Task scheduler lifecycle smoke passed. (completed: {}, failed: {}, canceled: {}, rejected: {}, long waits: {}, retained handles: {})",
					Diagnostics.CompletedTaskCount,
					Diagnostics.FailedTaskCount,
					Diagnostics.CanceledTaskCount,
					Diagnostics.RejectedTaskCount,
					Diagnostics.LongWaitCount,
					Diagnostics.RetainedTerminalHandleCount);
			}

			FThreadEvent AdmissionProbeStarted;
			FTaskHandle AdmissionProbe;
			FTaskHandle SlowTask;
			FTaskHandle ShortTask;
			FTaskHandle DependentTask;
			FTaskHandle FailedTask;
			FTaskHandle FailedDependentTask;
			FTaskHandle CancelableTask;
			FTaskHandle ParallelTask;
			FTaskHandle WaiterTask;
			FTaskHandle GameThreadSource;
			FTaskHandle GameThreadDeferred;
			FParallelForResult ParallelResult;
			ETaskState WaitedState = ETaskState::Invalid;
			uint64 ParallelChecksum = 0;
			bool bAdmissionRejected = false;
			bool bGameThreadDeferredRan = false;
		};
	}

	FEngineLoop GEngineLoop;

	auto FEngineLoop::PreInit(const FEngineStartupParams& Params) -> bool
	{
		DURIN_PROFILE_THREAD("GameThread");
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		GIsWindowDisplaySuppressed = Params.bSuppressWindowDisplay;
		bRunTaskSchedulerLifecycleSmoke =
			Params.bRunTaskSchedulerLifecycleSmoke;

		FPlatformMisc::EnableUserBinaryDirectoriesSearch();
		FPlatformMisc::AddRuntimeBinaryDirectory(FPaths::EngineThirdPartyRuntimeBinariesDir().c_str());

		PrepareRuntimeStorage();
		const std::filesystem::path SavedAppConfig = std::filesystem::path(FPaths::LaunchConfigsDir()) / AppConfigFileName;
		const std::filesystem::path LegacyAppConfig = std::filesystem::path(FPaths::LaunchDir()) / AppConfigFileName;
		LoadAppConfig((std::filesystem::exists(SavedAppConfig) ? SavedAppConfig : LegacyAppConfig).string());

		FNameInit(); // Initialize FName system.
		LoggerInit();
		for (const std::string& Warning : RuntimeStorageMigrationWarnings) DURIN_WARN("{}", Warning);
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
		checkf(
			PathUtilities::InitDefaultMountPoints(&MountError),
			"Failed to initialize mount registry: {}", MountError);
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
		if (!RHIInit())
		{
			DURIN_ERROR(
				"Engine initialization stopped because the dynamic RHI could not start.");
			ShutdownTaskSystem(ETaskShutdownMode::Drain);
			RemoveFromRoot(GEngine);
			MarkObjectHierarchyAsGarbage(GEngine);
			GEngine = nullptr;
			CollectGarbage();
			FModuleManager::Get().UnloadModulesAtShutdown();
			ShutdownApplicationCore();
			return false;
		}
		// Command admission must be running before Mona, the renderer, or editor
		// modules can publish their first render-thread work.
		InitRenderingThread();
		FModuleManager::Get().LoadModuleChecked("Mona");

		GEngine->Init();
		LastTickTime = FTime::Seconds();

		DURIN_INFO(STR("Durin engine initialized."));
		return true;
	}

	// Called from render thread
	static auto BeginFrameRenderThread(FRHICommandListImmediate& CommandList, uint64 LogicFrameCounter, uint64 RenderFrameCounter) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("RenderFrame.Begin");
		check(IsInRenderingThread());
		GFrameCounterRenderThread = LogicFrameCounter;
		GRenderFrameCounterRenderThread = RenderFrameCounter;
		CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
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
		PumpGameThreadDeferredWork();
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
		DURIN_PROFILE_FRAME_MARK();
	}

	auto FEngineLoop::Exit() -> void
	{
		std::unique_ptr<FEngineTaskSchedulerLifecycleSmoke> TaskSchedulerSmoke;
		if (bRunTaskSchedulerLifecycleSmoke)
		{
			TaskSchedulerSmoke =
				std::make_unique<FEngineTaskSchedulerLifecycleSmoke>();
			TaskSchedulerSmoke->Begin();
		}

		FModuleManager::Get().ShutdownModule("Mona");

		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		if (TaskSchedulerSmoke)
		{
			TaskSchedulerSmoke->ValidateAfterShutdown();
		}

		RemoveFromRoot(GEngine);
		MarkObjectHierarchyAsGarbage(GEngine);
		GEngine = nullptr;
		Asset::ShutdownAssetManager();
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
