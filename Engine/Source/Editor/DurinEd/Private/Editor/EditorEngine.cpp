#include "Editor/EditorEngine.h"
#include "Editor/Notification.h"
#include "Editor/Transaction.h"
#include "Editor/Transactor.h"

#include "Asset/Mutation.h"
#include "Asset.h"
#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "Actors/CameraActor.h"
#include "Components/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Console/ConsoleCommand.h"
#include "Engine/Level.h"
#include "Engine/ProjectGameSettings.h"
#include "Engine/World.h"
#include "Editor/EditorHost.h"
#include "Modules/ModuleManager.h"
#include "Misc/Project.h"
#include "Profiling/Profiling.h"
#include "RenderingThread.h"
#include "Mona.h"
#include "Client/SceneViewport.h"
#include "Client/ViewportClient.h"
#include "Application/MonaApplication.h"
#include "Widgets/MWindow.h"

namespace Durin
{
	DEditorEngine* GEditor = nullptr;

	DEditorEngine::DEditorEngine(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, NotificationManager(std::make_unique<Editor::FNotificationManager>())
	{
		Trans = NewObject<DTransBuffer>(this, "Transactor", EObjectFlags::Transient);
		GEditor = this;
	}

	DEditorEngine::~DEditorEngine()
	{
		if (GEditor == this) GEditor = nullptr;
	}

	auto DEditorEngine::Init(const FEngineInitContext& InitContext)
		-> FEngineInitializationResult
	{
		// The registry extracts references before MainFrame activates the full
		// editor stack. Publish editor-authored package classes first so import
		// records participate in that initial, atomic catalog revision.
		if (!FModuleManager::Get().LoadModule("AssetForgeBuiltins"))
			return FEngineInitializationResult::Failure(
				"Editor initialization requires AssetForgeBuiltins before the asset catalog scan.");
		if (FEngineInitializationResult Result = DEngine::Init(InitContext); !Result)
			return Result;
		EditorWorld = GetWorld();
		EditorWorld->SetWorldType(EWorldType::Editor);

		EditorHost =
			&FModuleManager::LoadModuleChecked<IEditorHost>("MainFrame");
		Profiling::SetStartupProjectMode(HasCurrentProject());
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::EditorShellBegin);
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.EditorShell");
			EditorHost->CreateEditorHost(InitContext.StartupWindow);
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::EditorShellComplete);

		if (!InitContext.PumpStartupFrame)
			return FEngineInitializationResult::Failure(
				"Editor initialization requires a startup-frame pump.");
		while (true)
		{
			if (!InitContext.PumpStartupFrame())
				return FEngineInitializationResult::Cancelled(
					"Editor initialization was cancelled by a close request.");
			const bool bFirstPresentAvailable = InitContext.bHeadless
				|| Profiling::GetStartupMilestoneMilliseconds(
					Profiling::EStartupMilestone::FirstPresent) >= 0.0;
			const Editor::Host::FBootstrapProgress Progress =
				EditorHost->AdvanceBootstrap(
					bFirstPresentAvailable);
			if (Progress.Status == Editor::Host::EBootstrapStepStatus::Ready) break;
			if (Progress.Status == Editor::Host::EBootstrapStepStatus::Failed)
			{
				// Keep the actionable failure visible for one final safe frame.
				InitContext.PumpStartupFrame();
				return FEngineInitializationResult::Failure(Progress.Message);
			}
		}
		Profiling::TryLogStartupTimingSummary();

		auto RegisterCommand = [this](FConsoleCommandDesc Desc) {
			if (const FConsoleCommandHandle Handle = FConsoleCommandRegistry::Get().RegisterCommand(std::move(Desc))) ConsoleCommandHandles.push_back(Handle);
		};
		RegisterCommand({"pie.play", "Starts Play from the level start.", "pie.play", [](std::span<const std::string> Args) {
			if (!Args.empty()) return FConsoleCommandResult::Failure("Usage: pie.play");
			if (!GEditor || !GEditor->GetEditorWorld()) return FConsoleCommandResult::Failure("The editor world is unavailable.");
			std::string Error;
			return GEditor->StartPlaySession(GEditor->GetEditorWorld()->GetCurrentLevel(), &Error) ? FConsoleCommandResult::Success("Play session started.") : FConsoleCommandResult::Failure(std::move(Error));
		}});
		RegisterCommand({"pie.stop", "Stops the active Play session.", "pie.stop", [](std::span<const std::string> Args) {
			if (!Args.empty()) return FConsoleCommandResult::Failure("Usage: pie.stop");
			if (!GEditor || !GEditor->IsPlaying()) return FConsoleCommandResult::Failure("No Play session is active.");
			GEditor->StopPlaySession();
			return FConsoleCommandResult::Success("Play session stopped.");
		}});
		RegisterCommand({"pie.pause", "Pauses or resumes Play.", "pie.pause [on|off|toggle]", [](std::span<const std::string> Args) {
			if (!GEditor || !GEditor->IsPlaying()) return FConsoleCommandResult::Failure("No Play session is active.");
			if (Args.size() > 1) return FConsoleCommandResult::Failure("Usage: pie.pause [on|off|toggle]");
			bool bPaused = !GEditor->IsPlaySessionPaused();
			if (!Args.empty() && Args[0] != "toggle")
			{
				if (Args[0] == "on") bPaused = true;
				else if (Args[0] == "off") bPaused = false;
				else return FConsoleCommandResult::Failure("Usage: pie.pause [on|off|toggle]");
			}
			GEditor->SetPlaySessionPaused(bPaused);
			return FConsoleCommandResult::Success(bPaused ? "Play paused." : "Play resumed.");
		}});
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
		RegisterCommand({"pie.status", "Shows the current Play state.", "pie.status", [](std::span<const std::string> Args) {
			if (!Args.empty()) return FConsoleCommandResult::Failure("Usage: pie.status");
			if (!GEditor || !GEditor->IsPlaying()) return FConsoleCommandResult::Success("Stopped");
			return FConsoleCommandResult::Success(GEditor->IsPlaySessionPaused() ? "Paused" : "Playing");
		}});
		DURIN_DEBUG("Editor initialized successfully");
		return FEngineInitializationResult::Success();
	}

	auto DEditorEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
		ReleaseRetiredPlaySessions();
		if (IsPlayingInNewWindow() && PlayWindow)
		{
			const auto& Windows = Mona::FMonaApplication::Get().GetWindows();
			if (std::ranges::find(Windows, PlayWindow) == Windows.end())
			{
				ReleasePlayMouseCapture();
				StopPlaySession();
			}
		}
		DEngine::Tick(DeltaSeconds, bIdleMode);
	}

	auto DEditorEngine::PrepareForShutdown() -> void
	{
		if (EditorHost) EditorHost->DestroyEditorHost();
	}

	auto DEditorEngine::BeginDestroy() -> void
	{
		if (EditorHost)
			EditorHost->DestroyEditorHost();
		TeardownPlaySession();
		if (Trans)
		{
			Trans->BeginDestroy();
			Trans = nullptr;
		}
		for (const uint64 Handle : ConsoleCommandHandles) FConsoleCommandRegistry::Get().UnregisterCommand(Handle);
		ConsoleCommandHandles.clear();
		EditorHost = nullptr;
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

	auto DEditorEngine::InitializePlayWindowViewportClient(
		const FViewportClient* SourceClient) -> void
	{
		PlayWindowViewportClient = std::make_unique<FViewportClient>();
		if (SourceClient != nullptr)
			PlayWindowViewportClient->SetViewSettings(SourceClient->GetViewSettings());
	}

	auto DEditorEngine::StartPlaySession(DLevel* SourceLevel, std::string* OutError) -> bool
	{
		Editor::FPlayRequest Request;
		Request.SourceLevel = SourceLevel;
		return StartPlaySession(Request, OutError);
	}

	auto DEditorEngine::StartPlaySession(const Editor::FPlayRequest& Request, std::string* OutError) -> bool
	{
		return StartPlaySessionInternal(Request, std::nullopt, OutError);
	}

	auto DEditorEngine::StartPlaySessionInternal(
		const Editor::FPlayRequest& Request,
		std::optional<DClass*> GameModeOverride,
		std::string* OutError) -> bool
	{
		DLevel* SourceLevel = Request.SourceLevel;
		if (OutError) OutError->clear();
		if (PlayState != Editor::EPlayState::Stopped)
		{
			if (OutError) *OutError = "A Play session is already active.";
			return false;
		}
		if (!EditorWorld || !SourceLevel || EditorWorld->GetCurrentLevel() != SourceLevel)
		{
			if (OutError) *OutError = "The current editor level is not available for Play.";
			return false;
		}

		PlayState = Editor::EPlayState::Starting;
		ReleasePlayMouseCapture();
		ClearGameInputWindow();
		DWorld* NewPlayWorld = NewObject<DWorld>(this, "PlayWorld");
		NewPlayWorld->SetWorldType(EWorldType::PlayInEditor);
		EditorToPlayObjects.clear();
		DLevel* PlayLevel = DuplicateObject(
			SourceLevel, NewPlayWorld,
			FName(std::format("{}_PIE", SourceLevel->GetName())),
			&EditorToPlayObjects);
		if (!PlayLevel)
		{
			MarkObjectHierarchyAsGarbage(NewPlayWorld);
			PlayState = Editor::EPlayState::Stopped;
			if (OutError) *OutError = "Could not duplicate the level for Play.";
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
					MarkObjectHierarchyAsGarbage(NewPlayWorld);
					PlayState = Editor::EPlayState::Stopped;
					if (OutError) *OutError = SettingsResult.Message;
					return false;
				}
				const FNativeGameModeResolution Resolution = ResolveNativeGameMode(Settings);
				if (!Resolution)
				{
					EditorToPlayObjects.clear();
					MarkObjectHierarchyAsGarbage(NewPlayWorld);
					PlayState = Editor::EPlayState::Stopped;
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
			MarkObjectHierarchyAsGarbage(NewPlayWorld);
			PlayState = Editor::EPlayState::Stopped;
			if (OutError) *OutError = "Could not activate the duplicated Play level.";
			return false;
		}
		NewPlayWorld->SetPhysicsSimulationEnabled(Request.bSimulatePhysics);
		AActor* ViewTargetOverride = nullptr;
		if (Request.StartLocation == Editor::EPlayStartLocation::EditorCamera)
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

		if (Request.Destination == Editor::EPlayDestination::NewWindow)
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

		(void)Trans->Reset();
		const FWorldPlayResult PlayResult = NewPlayWorld->BeginPlay({
			.GameModeClass = GameModeClass,
			.ViewTargetOverride = ViewTargetOverride});
		if (!PlayResult)
		{
			StopPlaySession();
			if (OutError) *OutError = PlayResult.Message;
			return false;
		}
		PlayState = Editor::EPlayState::Playing;
		return true;
	}

	auto DEditorEngine::StopPlaySession() -> void
	{
		if (PlayState == Editor::EPlayState::Stopped) return;
		DWorld* WorldToRestore = EditorWorld.Get();
		DLevel* LevelToRestore = EditorLevel.Get();
		TeardownPlaySession();
		SetWorld(WorldToRestore);
		if (WorldToRestore && LevelToRestore)
			WorldToRestore->SetCurrentLevel(LevelToRestore, false);
	}

	auto DEditorEngine::TeardownPlaySession() -> void
	{
		if (PlayState == Editor::EPlayState::Stopped) return;
		PlayState = Editor::EPlayState::Stopping;
		ReleasePlayMouseCapture();
		ClearGameInputWindow();
		DWorld* WorldToDestroy = PlayWorld.Get();
		DLevel* LevelToDestroy = WorldToDestroy ? WorldToDestroy->GetCurrentLevel() : nullptr;
		if (WorldToDestroy)
		{
			WorldToDestroy->EndPlay();
			WorldToDestroy->SetCurrentLevel(nullptr, false);
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
		if (PlayDestination == Editor::EPlayDestination::NewWindow)
		{
			SetMainSceneViewport(PreviousSceneViewport);
			if (PlayWindow)
			{
				const auto& Windows = Mona::FMonaApplication::Get().GetWindows();
				if (std::ranges::find(Windows, PlayWindow) != Windows.end()) Mona::FMonaApplication::Get().RequestDestroyWindow(PlayWindow);
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
		(void)Trans->Reset();
		PlayState = Editor::EPlayState::Stopped;
		PlayDestination = Editor::EPlayDestination::EmbeddedViewport;
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
			if (DWorld* World = RetiredPlayWorlds[Index].Get()) MarkObjectHierarchyAsGarbage(World);
			RetiredPlayFences.erase(RetiredPlayFences.begin() + Index);
			RetiredPlayLevels.erase(RetiredPlayLevels.begin() + Index);
			RetiredPlayWorlds.erase(RetiredPlayWorlds.begin() + Index);
		}
	}

	auto DEditorEngine::SetPlaySessionPaused(bool bPaused) -> void
	{
		if (PlayState != Editor::EPlayState::Playing && PlayState != Editor::EPlayState::Paused) return;
		if (bPaused) ReleasePlayMouseCapture();
		if (PlayWorld) PlayWorld->SetPaused(bPaused);
		PlayState = bPaused ? Editor::EPlayState::Paused : Editor::EPlayState::Playing;
	}

	auto DEditorEngine::StepPlaySession() -> void
	{
		if (PlayState != Editor::EPlayState::Paused || !PlayWorld) return;
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
		if (MouseCaptureState == Editor::EMouseCaptureState::Suspended)
			MouseCaptureState = Editor::EMouseCaptureState::Released;
		if (bCaptureClicked) RequestPlayMouseCapture(Window);
	}

	auto DEditorEngine::RequestPlayMouseCapture(const std::shared_ptr<FGenericWindow>& Window) -> bool
	{
		if (!Window || PlayState != Editor::EPlayState::Playing || !Window->IsFocused()) return false;
		if (GameInputWindow.lock() != Window) SetGameInputWindow(Window);
		if (MouseCaptureState == Editor::EMouseCaptureState::Captured
			&& CapturedMouseWindow.lock() == Window) return true;
		ReleasePlayMouseCapture();
		CapturedMouseWindow = Window;
		Window->SetCursorMode(ECursorMode::Captured);
		ResetGameInputMouse();
		SetGameInputEnabled(true);
		MouseCaptureState = Editor::EMouseCaptureState::Captured;
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
		MouseCaptureState = Editor::EMouseCaptureState::Released;
	}

	auto DEditorEngine::SuspendPlayMouseCapture() -> void
	{
		const bool bWasCaptured = MouseCaptureState == Editor::EMouseCaptureState::Captured;
		ReleasePlayMouseCapture();
		if (bWasCaptured) MouseCaptureState = Editor::EMouseCaptureState::Suspended;
	}

	auto DEditorEngine::HandleGameInputWindowFocus(
		const std::shared_ptr<FGenericWindow>& Window,
		bool bFocused) -> void
	{
		if (!bFocused && GameInputWindow.lock() == Window) SuspendPlayMouseCapture();
		else if (bFocused && MouseCaptureState == Editor::EMouseCaptureState::Suspended)
			MouseCaptureState = Editor::EMouseCaptureState::Released;
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
		if (Key != EKey::Escape || bRepeat || MouseCaptureState != Editor::EMouseCaptureState::Captured) return false;
		ReleasePlayMouseCapture();
		return true;
	}

	auto DEditorEngine::HandleGameInputMouseDown(
		const std::shared_ptr<FGenericWindow>& Window,
		EMouseButton Button) -> bool
	{
		if (!IsPlayingInNewWindow() || Button != EMouseButton::Left
			|| MouseCaptureState == Editor::EMouseCaptureState::Captured) return false;
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
			std::string CopyError;
			if (!CopyEditableObjectProperties(PlayObject, EditorObject, PlayToEditorObjects, &CopyError))
			{
				if (bCopiedAny)
				{
					(void)Trans->Reset();
					if (DPackage* Package = EditorLevel->GetPackage())
						Trans->InvalidateSavedState(*Package);
				}
				if (OutError) *OutError = std::move(CopyError);
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
		(void)Trans->Reset();
		if (DPackage* Package = EditorLevel->GetPackage())
			Trans->InvalidateSavedState(*Package);
		if (OutAppliedActorCount) *OutAppliedActorCount = static_cast<uint32>(SelectedRoots.size());
		return true;
	}

	auto DEditorEngine::GetNotificationManager() -> Editor::FNotificationManager&
	{
		return *NotificationManager;
	}

}
