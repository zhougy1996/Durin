#include "TickableEditorObject.h"
#include "Tickable.h"
#include "Editor/EditorEngine.h"
#include "ShaderPipelineCache.h"
#include "Tools/EditorToolSubsystem.h"
#include "Notifications/Notification.h"
#include "Notifications/EditorNotificationSubsystem.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Transactions/Transaction.h"
#include "Transactions/Transactor.h"

#include "Asset/Mutation.h"
#include "Asset/Asset.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Actors/CameraActor.h"
#include "Components/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Console/ConsoleCommand.h"
#include "Engine/Level.h"
#include "Engine/ProjectGameSettings.h"
#include "Engine/World.h"
#include "Editor/IMainFrameModule.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModularFeature.h"
#include "Threading/RunnableThread.h"
#include "Project/Project.h"
#include "Profiling/Profiling.h"
#include "RenderingThread.h"
#include "Mona.h"
#include "Client/SceneViewport.h"
#include "Client/ViewportClient.h"
#include "Application/MonaApplication.h"
#include "Widgets/MWindow.h"
#include "Rendering/MonaRenderer.h"

namespace Durin
{
	DEditorEngine* GEditor = nullptr;

	DEditorEngine::DEditorEngine(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer), EditorSubsystems(*this)
	{
		Trans = NewObject<DTransBuffer>(this, "Transactor", EObjectFlags::Transient);
	}

	DEditorEngine::~DEditorEngine()
	{
		if (GEditor == this) GEditor = nullptr;
	}
	auto DEditorEngine::ActivateEditor() -> bool
	{
		CheckGameThread();
		if (bShutdownRequested || !IsValid(this) || GEditor) return false;
		GEditor = this;
		return true;
	}

	auto DEditorEngine::BeginTransaction(std::string_view Description) -> int32
	{ return BeginTransaction({}, Description); }
	auto DEditorEngine::BeginTransaction(std::string_view Context, std::string_view Description, DObject* PrimaryObject) -> int32
	{
		if (!CanTransact()) return -1;
		const auto Index = Trans->Begin(Context, Description);
		if (Index >= 0) Trans->SetPrimaryUndoObject(PrimaryObject);
		return Index;
	}
	auto DEditorEngine::EndTransaction() -> int32 { return Trans ? Trans->End() : 0; }
	auto DEditorEngine::GetCapabilities() const -> FEditorCapabilities
	{
		const bool bReady = !bShutdownRequested && GetStartupState() == EHostStartupState::Ready;
		const bool bRunTools = bReady && !IsPreparingMapChange();
		return {
			.bTick = bReady,
			.bRunTools = bRunTools,
			.bEditWorld = bRunTools && !IsPlaying(),
			.bPresentWorkspace = bReady};
	}
	auto DEditorEngine::CanTransact() const -> bool
	{ return GEditor == this && GetCapabilities().bRunTools && Trans && Trans->CanTransact(); }
	auto DEditorEngine::CancelTransaction(int32 StartIndex) -> void { if (Trans) Trans->Cancel(StartIndex); }

	auto DEditorEngine::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		Super::AddReferencedObjects(Collector);
		EditorSubsystems.AddReferencedObjects(Collector);
	}

	auto DEditorEngine::InitializeEditorSubsystems() -> void
	{
		CheckGameThread();
		requiref(GEditor == this && !bShutdownRequested && IsValid(this), "Editor subsystem initialization requires the active editor.");

		FModuleManager::Get().LoadModuleChecked("DurinEd");
		if (!bShutdownRequested) EditorSubsystems.Initialize();
	}

	auto DEditorEngine::Init(const FEngineInitContext& Context) -> bool
	{
		CheckGameThread();
		if (bEditorInitStarted || GEditor != this || bShutdownRequested || !IsValid(this))
		{
			DURIN_ERROR("Editor initialization requires the active editor and is one-shot.");
			return false;
		}
		bEditorInitStarted = true;
		StartupState = EHostStartupState::Pending;
		StartupError = Context.ProjectAdmissionError;

		bool bInitialized = InitEditorInternal(Context);
		if (bShutdownRequested && bInitialized)
		{
			DURIN_ERROR("Editor retired during initialization.");
			bInitialized = false;
		}
		if (!bInitialized) RequestShutdown();
		return bInitialized;
	}

	auto DEditorEngine::InitEditorInternal(const FEngineInitContext& InitContext)
		-> bool
	{
		FShaderPipelineCache::SetBatchMode(FShaderPipelineCache::BatchMode::Fast);
		if (!DEngine::Init(InitContext))
			return false;
		EditorWorld = GetWorld();

		MainFrameModule =
			&FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
		FModuleManager::Get().LoadModuleChecked("DurinMCP");
		InitializeEditorSubsystems();
		if (bShutdownRequested) return false;
		Profiling::SetStartupProjectMode(HasCurrentProject());
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::EditorShellBegin);
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.EditorShell");
			MainFrameModule->CreateDefaultMainFrame(*this, InitContext.StartupWindow);
			StartupWindow = InitContext.StartupWindow;
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::EditorShellComplete);


		auto RegisterCommand = [this](FConsoleCommandDesc Desc) {
			if (const FConsoleCommandHandle Handle = FConsoleCommandRegistry::Get().RegisterCommand(std::move(Desc))) ConsoleCommandHandles.push_back(Handle);
		};
		RegisterCommand({"pie.step", "Advances one frame while Play is paused.", "pie.step", [](std::span<const std::string> Args) {
			if (!Args.empty() || !GEditor || !GEditor->IsPlaySessionPaused()) return FConsoleCommandResult::Failure("pie.step requires a paused Play session.");
			GEditor->StepPlaySession();
			return FConsoleCommandResult::Success("Single step requested.");
		}});
		RegisterCommand({"pie.physics", "Enables or disables physics in Play.", "pie.physics <on|off>", [](std::span<const std::string> Args) {
			if (!GEditor || !GEditor->GetPlayWorld() || Args.size() != 1 || (Args[0] != "on" && Args[0] != "off")) return FConsoleCommandResult::Failure("Usage: pie.physics <on|off>");
			const bool bEnabled = Args[0] == "on";
			GEditor->GetPlayWorld()->SetPhysicsSimulationEnabled(bEnabled);
			return FConsoleCommandResult::Success(bEnabled ? "Physics enabled." : "Physics disabled.");
		}});
		RegisterCommand({"pie.apply", "Applies editable runtime values to all source actors.", "pie.apply", [](std::span<const std::string> Args) {
			if (!Args.empty() || !GEditor) return FConsoleCommandResult::Failure("Usage: pie.apply");
			uint32 AppliedCount = 0;
			std::string Error;
			return GEditor->ApplyPlaySessionChanges({}, &AppliedCount, &Error)
				? FConsoleCommandResult::Success(std::format("Applied runtime changes from {} actor(s).", AppliedCount))
				: FConsoleCommandResult::Failure(std::move(Error));
		}});
		const bool bWorkspaceReady = InitializeProjectWorkspace();
		if (bShutdownRequested) return false;
		StartupState = bWorkspaceReady ? EHostStartupState::Ready : EHostStartupState::Failed;
		if (bWorkspaceReady)
		{
			Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::HostReady);
			if (HasCurrentProject()) OpenDefaultLevel();
		}
		FShaderPipelineCache::SetBatchMode(FShaderPipelineCache::BatchMode::Background);
		Profiling::TryLogStartupTimingSummary();
		DURIN_DEBUG("Editor services initialized");
		return true;
	}

	auto DEditorEngine::GetStartupCompletion(std::string& OutError) const -> EHostStartupState
	{
		OutError = StartupError;
		if (bShutdownRequested) return EHostStartupState::Pending;
		const auto State = GetStartupState();
		if (State != EHostStartupState::Ready) return State;
		if (!StartupError.empty()) return EHostStartupState::Failed;
		if (InitialDocumentRequest)
		{
			const auto Result = GetEditorSubsystem<DAssetEditorSubsystem>()->ResolveOpenResult(*InitialDocumentRequest);
			if (Result.Status == EAssetOpenStatus::Pending) return EHostStartupState::Pending;
			if (Result.Status != EAssetOpenStatus::Completed)
			{
				OutError = Result.Message.empty() ? "Could not open the initial document." : Result.Message;
				return EHostStartupState::Failed;
			}
			if (Result.Target.AssetPath.IsValid() && !GetEditorSubsystem<DAssetEditorSubsystem>()->IsAssetOpen(Result.Target.AssetPath))
			{
				OutError = "The initial document was closed before its first presentation.";
				return EHostStartupState::Failed;
			}
		}
		if (GetWorld() && GetWorld()->GetCurrentLevel())
		{
			if (!MainSceneViewport) return EHostStartupState::Pending;
			const auto Window = StartupWindow.lock();
			const auto Presentation = MainSceneViewport->QueryPresentation(GetSceneGeneration(), Window.get());
			if (Presentation == EScenePresentationState::Failed)
			{
				OutError = "The committed initial scene could not render for primary presentation.";
				return EHostStartupState::Failed;
			}
			if (Presentation != EScenePresentationState::Presented) return EHostStartupState::Pending;
		}
		else if (const auto Window = StartupWindow.lock())
		{
			const auto* Renderer = Mona::FMonaApplication::IsInitialized()
				? Mona::FMonaApplication::Get().GetRenderer() : nullptr;
			if (!Renderer || !Renderer->IsWindowPresented(*Window)) return EHostStartupState::Pending;
		}
		else return EHostStartupState::Pending;
		return EHostStartupState::Ready;
	}

	auto DEditorEngine::AdoptSceneWorld(DWorld* World, FScenePtr Scene, uint64 Generation) -> void
	{
		check(!IsPlaying());
		Super::AdoptSceneWorld(World, std::move(Scene), Generation);
		EditorWorld = World;
	}

	auto DEditorEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
		if (bShutdownRequested || !std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0f) return;
		if (!GetCapabilities().bTick) return;

		FTickableEditorObject::TickObjects(*this, DeltaSeconds);
		if (bShutdownRequested) return;

		if (auto* Tools = GetCapabilities().bRunTools ? GetEditorSubsystem<DEditorToolSubsystem>() : nullptr)
		{
			Tools->TickTools();
			if (bShutdownRequested) return;
			Tools->TickScripts();
		}
		if (bShutdownRequested) return;
		ReleaseRetiredPlaySessions();
		if (IsPlayingInNewWindow() && PlayWindow)
		{
			const auto& Windows = Mona::FMonaApplication::Get().GetWindows();
			if (!std::ranges::contains(Windows, PlayWindow))
			{
				ReleasePlayMouseCapture();
				StopPlaySession();
			}
		}
		bool bAWorldTicked = false;
		ELevelTick TickType = LEVELTICK_ViewportsOnly;
		if (EditorWorld && EditorWorld.Get() != GetWorld())
		{
			EditorWorld->Tick({.DeltaSeconds = DeltaSeconds});
			bAWorldTicked = true;
		}
		if (bShutdownRequested) return;
		if (auto* World = GetWorld())
		{
			bAWorldTicked = true;
			if (World->GetWorldType() == EWorldType::Game || World->GetWorldType() == EWorldType::PIE)
				TickType = LEVELTICK_All;
		}
		DEngine::Tick(DeltaSeconds, bIdleMode);
		if (bShutdownRequested) return;
		if (bAWorldTicked) FTickableGameObject::TickObjects(nullptr, TickType, false, DeltaSeconds);
		if (bShutdownRequested) return;
		if (auto* AssetEditors = GetEditorSubsystem<DAssetEditorSubsystem>()) AssetEditors->HandleTicker(DeltaSeconds);
		if (bShutdownRequested) return;
		if (MainFrameModule) MainFrameModule->TickMainFrame(DeltaSeconds);
		if (bShutdownRequested) return;
		if (auto* Notifications = GetEditorSubsystem<DEditorNotificationSubsystem>()) Notifications->Tick(DeltaSeconds);
	}

	auto DEditorEngine::CheckEditorSubsystemUpdate() -> void
	{
		CheckGameThread();
		requiref(GEditor == this && !bShutdownRequested
			&& !IsGarbageCollecting() && !FModularFeatureRegistry::Get().IsDispatching(),
			"Editor subsystem update requires an active host outside GC and feature dispatch.");
	}
	auto DEditorEngine::AddRegisteredEditorSubsystems() -> void
	{
		CheckEditorSubsystemUpdate();

		EditorSubsystems.AddRegisteredSubsystems();
	}

	auto DEditorEngine::AddEditorSubsystem(DClass* Type) -> DEditorSubsystem*
	{
		CheckEditorSubsystemUpdate();

		return EditorSubsystems.AddSubsystem(Type);
	}
	auto DEditorEngine::RemoveEditorSubsystem(DClass* Type) -> void
	{
		CheckEditorSubsystemUpdate();

		EditorSubsystems.RemoveSubsystem(Type);
	}
	auto DEditorEngine::RemoveEditorSubsystems(FName Provider) -> void
	{
		// Startup rollback and completed host teardown have no live instances to remove.
		if (!EditorSubsystems.HasProvider(Provider)) return;
		CheckEditorSubsystemUpdate();

		EditorSubsystems.RemoveSubsystems(Provider);
	}

	auto DEditorEngine::RequestSubsystemDeinitialization() -> void
	{
		// These consumers own their asynchronous work and retained callbacks.
		if (auto* Tools = GetEditorSubsystem<DEditorToolSubsystem>()) Tools->RequestShutdown();
		if (auto* Notifications = GetEditorSubsystem<DEditorNotificationSubsystem>()) Notifications->RequestShutdown();
		if (EditorWorld) EditorWorld->BeginTearingDown();
		if (PlayWorld) PlayWorld->BeginTearingDown();
		Super::RequestSubsystemDeinitialization();
	}

	auto DEditorEngine::RetireHostConsumers() -> void
	{
		TeardownPlaySession();
		if (Trans) Trans->Cancel();
		if (EditorWorld) EditorWorld->CleanupWorld();
		StartupWindow.reset();
		InitialDocumentRequest.reset();
		if (MainFrameModule) MainFrameModule->DestroyMainFrame();
		EditorSubsystems.Deinitialize();
	}

	auto DEditorEngine::PreExit() -> void
	{
		requiref(!EditorSubsystems.IsPopulating(), "PreExit cannot run from subsystem lifecycle callbacks.");
		FShaderPipelineCache::SetBatchMode(FShaderPipelineCache::BatchMode::Background);
		Super::PreExit();
		if (bShutdownComplete && GEditor == this) GEditor = nullptr;
	}

	auto DEditorEngine::BeginDestroy() -> void
	{
		PreExit();
		if (Trans)
		{
			Trans->BeginDestroy();
			Trans = nullptr;
		}
		for (const uint64 Handle : ConsoleCommandHandles) FConsoleCommandRegistry::Get().UnregisterCommand(Handle);
		ConsoleCommandHandles.clear();
		MainFrameModule = nullptr;
		StartupWindow.reset();
		InitialDocumentRequest.reset();
		EditorLevel = nullptr;
		EditorWorld = nullptr;
		DEngine::BeginDestroy();
		// The base teardown drains the rendering thread before releasing the scene.
		ReleaseRetiredPlaySessions(true);
	}

	auto DEditorEngine::GetPlayViewportRenderSettingsClient() -> FViewportClient*
	{
		return PlayWindowViewportClient.get();
	}
	auto DEditorEngine::GetEditorWorldContext() const -> const FWorldContext&
	{
		const auto* Context = GetWorldContextFromWorld(EditorWorld.Get());
		checkf(Context, "The editor World is not initialized.");
		return *Context;
	}
	auto DEditorEngine::GetPIEWorldContext() const -> const FWorldContext*
	{ return GetWorldContextFromWorld(PlayWorld.Get()); }

	auto DEditorEngine::InitializePlayWindowViewportClient(
		const FViewportClient* SourceClient) -> void
	{
		PlayWindowViewportClient = std::make_unique<FViewportClient>();
		if (SourceClient != nullptr)
			PlayWindowViewportClient->SetViewSettings(SourceClient->GetViewSettings());
	}

	auto DEditorEngine::StartPlaySession(DLevel* SourceLevel, std::string* OutError) -> bool
	{
		FPlayRequest Request;
		Request.SourceLevel = SourceLevel;
		return StartPlaySession(Request, OutError);
	}

	auto DEditorEngine::StartPlaySession(const FPlayRequest& Request, std::string* OutError) -> bool
	{
		return StartPlaySessionInternal(Request, std::nullopt, OutError);
	}

	auto DEditorEngine::StartPlaySessionInternal(
		const FPlayRequest& Request,
		std::optional<DClass*> GameModeOverride,
		std::string* OutError) -> bool
	{
		if (!GetCapabilities().bRunTools) return false;

		DLevel* SourceLevel = Request.SourceLevel;
		if (OutError) OutError->clear();
		if (PlayState != EPlayState::Stopped)
		{
			if (OutError) *OutError = "A Play session is already active.";
			return false;
		}
		if (!EditorWorld || !SourceLevel || EditorWorld->GetCurrentLevel() != SourceLevel)
		{
			if (OutError) *OutError = "The current editor level is not available for Play.";
			return false;
		}
		if (!Trans || !Trans->CanTransact())
		{
			if (OutError) *OutError = "Play cannot start while transaction history is busy or requires recovery.";
			return false;
		}

		PlayState = EPlayState::Starting;
		ReleasePlayMouseCapture();
		ClearGameInputWindow();
		// A dedicated package identifies this graph independently of runtime world type.
		auto* PlayPackage = NewObject<DPackage>(nullptr, "PlaySession", EObjectFlags::Transient);
		TStrongObjectPtr<DPackage> PackageGuard(PlayPackage);
		PlayPackage->SetPlayInEditor(true);
		DWorld* NewPlayWorld = NewObject<DWorld>(PlayPackage, "PlayWorld");
		TStrongObjectPtr<DWorld> ConstructionGuard(NewPlayWorld);
		NewPlayWorld->SetWorldType(EWorldType::PIE);
		NewPlayWorld->SetRenderScene(GetMainScene());
		struct FInitializationCleanup
		{
			DWorld* World;
			DPackage* Package;
			EPlayState& State;
			~FInitializationCleanup()
			{
				if (!World) return;
				World->CleanupWorld();
				MarkObjectHierarchyAsGarbage(Package);
				State = EPlayState::Stopped;
			}
		} InitializationCleanup{NewPlayWorld, PlayPackage, PlayState};
		NewPlayWorld->InitializeSubsystems(this);
		if (bShutdownRequested || NewPlayWorld->IsShutdownRequested())
		{
			if (OutError) *OutError = "Play World retired during initialization.";
			return false;
		}
		InitializationCleanup.World = nullptr;
		EditorToPlayObjects.clear();
		const auto Duplicated = DuplicateObject(
			SourceLevel, NewPlayWorld,
			FName(std::format("{}_PIE", SourceLevel->GetName())),
			&EditorToPlayObjects);
		DLevel* PlayLevel = Duplicated.value();
		if (!PlayLevel)
		{
			NewPlayWorld->CleanupWorld();
			MarkObjectHierarchyAsGarbage(PlayPackage);
			PlayState = EPlayState::Stopped;
			if (OutError) *OutError = ToString(Duplicated.error());
			return false;
		}
		DClass* GameModeClass = GameModeOverride.value_or(nullptr);
		if (!GameModeOverride)
		{
			if (const FProjectInfo* Project = GetCurrentProject())
			{
				FProjectGameSettings Settings;
				const FProjectGameSettingsResult SettingsResult =
					FProjectGameSettingsStore::ForProject(*Project).Load(Settings);
				if (!SettingsResult)
				{
					EditorToPlayObjects.clear();
					NewPlayWorld->CleanupWorld();
					MarkObjectHierarchyAsGarbage(PlayPackage);
					PlayState = EPlayState::Stopped;
					if (OutError) *OutError = SettingsResult.Message;
					return false;
				}
				const FNativeGameModeResolution Resolution = ResolveNativeGameMode(Settings);
				if (!Resolution)
				{
					EditorToPlayObjects.clear();
					NewPlayWorld->CleanupWorld();
					MarkObjectHierarchyAsGarbage(PlayPackage);
					PlayState = EPlayState::Stopped;
					if (OutError) *OutError = Resolution.Result.Message;
					return false;
				}
				GameModeClass = Resolution.GameModeClass;
			}
		}

		EditorLevel = SourceLevel;
		PlayWorld = NewPlayWorld;
		PlayDestination = Request.Destination;
		PlayToEditorObjects.clear();
		for (const auto& [EditorObject, PlayObject] : EditorToPlayObjects) PlayToEditorObjects.emplace(PlayObject, EditorObject);
		EditorWorld->SetCurrentLevel(nullptr, false);
		SetWorld(NewPlayWorld);
		if (!NewPlayWorld->SetCurrentLevel(PlayLevel))
		{
			SetWorld(EditorWorld.Get());
			EditorWorld->SetCurrentLevel(EditorLevel.Get());
			PlayWorld = nullptr;
			EditorLevel = nullptr;
			EditorToPlayObjects.clear();
			PlayToEditorObjects.clear();
			NewPlayWorld->CleanupWorld();
			MarkObjectHierarchyAsGarbage(PlayPackage);
			PlayState = EPlayState::Stopped;
			if (OutError) *OutError = "Could not activate the duplicated Play level.";
			return false;
		}
		NewPlayWorld->SetPhysicsSimulationEnabled(Request.bSimulatePhysics);
		AActor* ViewTargetOverride = nullptr;
		if (Request.StartLocation == EPlayStartLocation::EditorCamera)
		{
			ACameraActor* Camera = PlayLevel->SpawnActor<ACameraActor>("PIE_EditorCamera");
			if (!Camera || !Camera->GetCameraComponent())
			{
				StopPlaySession();
				if (OutError) *OutError = "Could not create the Play From Camera viewpoint.";
				return false;
			}
			Camera->GetCameraComponent()->SetLookAt(Request.CameraLocation, Request.CameraTarget);
			PlayLevel->SetPrimaryCameraActor(Camera);
			ViewTargetOverride = Camera;
		}

		if (Request.Destination == EPlayDestination::NewWindow)
		{
			PreviousSceneViewport = GetMainSceneViewport();
			InitializePlayWindowViewportClient(
				PreviousSceneViewport ? PreviousSceneViewport->GetViewportClient() : nullptr);
			PlayWindow = std::make_shared<MWindow>();
			PlayWindow->SetTitle("Durin Play");
			PlayWindow->ReshapeWindow({160.0f, 160.0f}, {1280.0f, 720.0f});
			Mona::FMonaApplication::Get().AddWindow(PlayWindow, true);
			Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(PlayWindow);
			PlayWindowViewport = FSceneViewport::CreateWindowBacked(
				PlayWindowViewportClient.get(), PlayWindow);
			SetMainSceneViewport(PlayWindowViewport);
			SetGameInputWindow(PlayWindow->GetNativeWindow());
		}

		if (const auto Reset = Trans->TryReset(); !Reset)
		{
			StopPlaySession();
			if (OutError) *OutError = FormatTransactorResult(Reset);
			return false;
		}
		const FWorldPlayResult PlayResult = NewPlayWorld->BeginPlay({
			.GameModeClass = GameModeClass,
			.ViewTargetOverride = ViewTargetOverride});
		if (!PlayResult)
		{
			StopPlaySession();
			if (OutError) *OutError = PlayResult.Message;
			return false;
		}
		PlayState = EPlayState::Playing;
		return true;
	}

	auto DEditorEngine::StopPlaySession() -> void
	{
		if (PlayState == EPlayState::Stopped) return;
		DWorld* WorldToRestore = EditorWorld.Get();
		DLevel* LevelToRestore = EditorLevel.Get();
		TeardownPlaySession();
		SetWorld(WorldToRestore);
		if (WorldToRestore && LevelToRestore)
			WorldToRestore->SetCurrentLevel(LevelToRestore, false);
	}

	auto DEditorEngine::TeardownPlaySession() -> void
	{
		if (PlayState == EPlayState::Stopped) return;
		PlayState = EPlayState::Stopping;
		ReleasePlayMouseCapture();
		ClearGameInputWindow();
		DWorld* WorldToDestroy = PlayWorld.Get();
		DLevel* LevelToDestroy = WorldToDestroy ? WorldToDestroy->GetCurrentLevel() : nullptr;
		if (WorldToDestroy)
		{
			WorldToDestroy->EndPlay();
			WorldToDestroy->SetCurrentLevel(nullptr, false);
			WorldToDestroy->CleanupWorld();
		}
		std::unique_ptr<FRenderCommandFence> RetirementFence;
		if (WorldToDestroy && GRenderingThread)
		{
			// Do not make the UI thread wait for Vulkan here. The retired object graph
			// keeps proxy source data alive until this fence passes the queued removals.
			RetirementFence = std::make_unique<FRenderCommandFence>();
			RetirementFence->BeginFence();
		}
		SetWorld(nullptr);
		if (PlayDestination == EPlayDestination::NewWindow)
		{
			SetMainSceneViewport(PreviousSceneViewport);
			if (PlayWindow)
			{
				const auto& Windows = Mona::FMonaApplication::Get().GetWindows();
				if (std::ranges::contains(Windows, PlayWindow)) Mona::FMonaApplication::Get().RequestDestroyWindow(PlayWindow);
			}
			PlayWindowViewport.reset();
			PlayWindowViewportClient.reset();
			PlayWindow.reset();
			PreviousSceneViewport.reset();
		}
		PlayWorld = nullptr;
		EditorLevel = nullptr;
		EditorToPlayObjects.clear();
		PlayToEditorObjects.clear();
		if (WorldToDestroy)
		{
			RetiredPlayWorlds.emplace_back(WorldToDestroy);
			RetiredPlayLevels.emplace_back(LevelToDestroy);
			RetiredPlayFences.emplace_back(std::move(RetirementFence));
		}
		Trans->Reset();
		PlayState = EPlayState::Stopped;
		PlayDestination = EPlayDestination::EmbeddedViewport;
		ReleaseRetiredPlaySessions();
	}

	auto DEditorEngine::ReleaseRetiredPlaySessions(bool bReleaseAll) -> void
	{
		check(RetiredPlayWorlds.size() == RetiredPlayLevels.size());
		check(RetiredPlayWorlds.size() == RetiredPlayFences.size());
		for (size_t Index = RetiredPlayFences.size(); Index-- > 0;)
		{
			const FRenderCommandFence* Fence = RetiredPlayFences[Index].get();
			if (!bReleaseAll && Fence && !Fence->IsFenceComplete()) continue;
			if (DWorld* World = RetiredPlayWorlds[Index].Get()) MarkObjectHierarchyAsGarbage(World->GetPackage() ? static_cast<DObject*>(World->GetPackage()) : World);
			RetiredPlayFences.erase(RetiredPlayFences.begin() + Index);
			RetiredPlayLevels.erase(RetiredPlayLevels.begin() + Index);
			RetiredPlayWorlds.erase(RetiredPlayWorlds.begin() + Index);
		}
	}

	auto DEditorEngine::SetPlaySessionPaused(bool bPaused) -> void
	{
		if (PlayState != EPlayState::Playing && PlayState != EPlayState::Paused) return;
		if (bPaused) ReleasePlayMouseCapture();
		if (PlayWorld) PlayWorld->SetPaused(bPaused);
		PlayState = bPaused ? EPlayState::Paused : EPlayState::Playing;
	}

	auto DEditorEngine::StepPlaySession() -> void
	{
		if (PlayState != EPlayState::Paused || !PlayWorld) return;
		ReleasePlayMouseCapture();
		PlayWorld->RequestSingleStep();
	}

	auto DEditorEngine::UpdateEmbeddedPlayMouseTarget(
		const std::shared_ptr<FGenericWindow>& Window,
		bool bFocused,
		bool bCaptureClicked) -> void
	{
		if (!IsPlaying() || IsPlayingInNewWindow()) return;
		if (!Window)
		{
			ReleasePlayMouseCapture();
			ClearGameInputWindow();
			return;
		}
		SetGameInputWindow(Window);
		if (!bFocused)
		{
			SuspendPlayMouseCapture();
			return;
		}
		if (MouseCaptureState == EMouseCaptureState::Suspended)
			MouseCaptureState = EMouseCaptureState::Released;
		if (bCaptureClicked) RequestPlayMouseCapture(Window);
	}

	auto DEditorEngine::RequestPlayMouseCapture(const std::shared_ptr<FGenericWindow>& Window) -> bool
	{
		if (!Window || PlayState != EPlayState::Playing || !Window->IsFocused()) return false;
		if (GameInputWindow.lock() != Window) SetGameInputWindow(Window);
		if (MouseCaptureState == EMouseCaptureState::Captured
			&& CapturedMouseWindow.lock() == Window) return true;
		ReleasePlayMouseCapture();
		CapturedMouseWindow = Window;
		Window->SetCursorMode(ECursorMode::Captured);
		ResetGameInputMouse();
		SetGameInputEnabled(true);
		MouseCaptureState = EMouseCaptureState::Captured;
		return true;
	}

	auto DEditorEngine::ReleasePlayMouseCapture() -> void
	{
		if (const std::shared_ptr<FGenericWindow> Window = CapturedMouseWindow.lock())
		{
			Window->SetCursorMode(ECursorMode::Free);
		}
		CapturedMouseWindow.reset();
		SetGameInputEnabled(false);
		MouseCaptureState = EMouseCaptureState::Released;
	}

	auto DEditorEngine::SuspendPlayMouseCapture() -> void
	{
		const bool bWasCaptured = MouseCaptureState == EMouseCaptureState::Captured;
		ReleasePlayMouseCapture();
		if (bWasCaptured) MouseCaptureState = EMouseCaptureState::Suspended;
	}

	auto DEditorEngine::HandleGameInputWindowFocus(
		const std::shared_ptr<FGenericWindow>& Window,
		bool bFocused) -> void
	{
		if (!bFocused && GameInputWindow.lock() == Window) SuspendPlayMouseCapture();
		else if (bFocused && MouseCaptureState == EMouseCaptureState::Suspended)
			MouseCaptureState = EMouseCaptureState::Released;
	}

	auto DEditorEngine::HandleGameInputWindowClose(const std::shared_ptr<FGenericWindow>& Window) -> void
	{
		if (GameInputWindow.lock() == Window) ReleasePlayMouseCapture();
	}

	auto DEditorEngine::HandleGameInputKeyDown(
		const std::shared_ptr<FGenericWindow>&,
		EKey Key,
		bool bRepeat) -> bool
	{
		if (Key != EKey::Escape || bRepeat || MouseCaptureState != EMouseCaptureState::Captured) return false;
		ReleasePlayMouseCapture();
		return true;
	}

	auto DEditorEngine::HandleGameInputMouseDown(
		const std::shared_ptr<FGenericWindow>& Window,
		EMouseButton Button) -> bool
	{
		if (!IsPlayingInNewWindow() || Button != EMouseButton::Left
			|| MouseCaptureState == EMouseCaptureState::Captured) return false;
		return RequestPlayMouseCapture(Window);
	}

	auto DEditorEngine::GetEditorObjectForPlayObject(const DObject* PlayObject) const -> DObject*
	{
		const auto It = PlayToEditorObjects.find(const_cast<DObject*>(PlayObject));
		return It == PlayToEditorObjects.end() ? nullptr : It->second;
	}

	auto DEditorEngine::ApplyPlaySessionChanges(const std::vector<AActor*>& PlayActors, uint32* OutAppliedActorCount, std::string* OutError) -> bool
	{
		if (OutAppliedActorCount) *OutAppliedActorCount = 0;
		if (OutError) OutError->clear();
		if (!IsPlaying() || !PlayWorld || !EditorLevel)
		{
			if (OutError) *OutError = "No Play session is active.";
			return false;
		}
		if (!Trans || !Trans->CanTransact())
		{
			if (OutError) *OutError = "Play changes cannot be applied while transaction history is busy or requires recovery.";
			return false;
		}

		std::unordered_set<DObject*> SelectedRoots;
		if (PlayActors.empty())
		{
			for (const TObjectPtr<AActor>& Actor : PlayWorld->GetActors()) if (GetEditorObjectForPlayObject(Actor.Get())) SelectedRoots.insert(Actor.Get());
		}
		else
		{
			for (AActor* Actor : PlayActors) if (Actor && PlayWorld->ContainsActor(Actor) && GetEditorObjectForPlayObject(Actor)) SelectedRoots.insert(Actor);
		}
		if (SelectedRoots.empty())
		{
			if (OutError) *OutError = "The selection contains no actors originating from the editor level.";
			return false;
		}

		auto IsUnderSelectedActor = [&](DObject* Object) {
			for (DObject* Current = Object; Current; Current = Current->GetOuter()) if (SelectedRoots.contains(Current)) return true;
			return false;
		};
		bool bCopiedAny = false;
		for (const auto& [PlayObject, EditorObject] : PlayToEditorObjects)
		{
			if (!IsUnderSelectedActor(PlayObject)) continue;
			if (const auto CopyResult = CopyEditableObjectProperties(PlayObject, EditorObject, PlayToEditorObjects); !CopyResult)
			{
				if (bCopiedAny)
				{
					Trans->Reset();
					if (DPackage* Package = EditorLevel->GetPackage())
						Trans->InvalidateSavedState(*Package);
				}
				if (OutError) *OutError = ToString(CopyResult.error());
				return false;
			}
			bCopiedAny = true;
		}
		for (DObject* PlayActor : SelectedRoots)
		{
			if (auto* EditorActor = Cast<AActor>(GetEditorObjectForPlayObject(PlayActor)))
			{
				if (DSceneComponent* Root = EditorActor->GetRootComponent()) Root->UpdateComponentToWorld();
				EditorActor->MarkPackageDirty();
			}
		}
		const auto Reset = Trans->TryReset();
		if (DPackage* Package = EditorLevel->GetPackage())
			Trans->InvalidateSavedState(*Package);
		if (!Reset)
		{
			if (OutError) *OutError = FormatTransactorResult(Reset);
			return false;
		}
		if (OutAppliedActorCount) *OutAppliedActorCount = static_cast<uint32>(SelectedRoots.size());
		return true;
	}

	auto DEditorEngine::GetNotificationManager() -> FNotificationManager&
	{
		auto* Service = GetEditorSubsystem<DEditorNotificationSubsystem>();
		require(Service);
		return Service->GetManager();
	}

}

namespace Durin
{
    auto DEditorEngine::GetEditorSubsystem(DClass* Type) const -> DEditorSubsystem*
    {
        if (!IsInGameThread() || !IsValid(Type) || !Type->IsChildOf(DEditorSubsystem::StaticClass())) return nullptr;
        return static_cast<DEditorSubsystem*>(EditorSubsystems.Find(Type));
    }
}
