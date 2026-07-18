#include "Editor/EditorEngine.h"
#include "Editor/EditorNotification.h"
#include "Editor/EditorTransaction.h"

#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "Actors/CameraActor.h"
#include "Components/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Console/ConsoleCommand.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Interfaces/IMainFrameModule.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "Mona.h"
#include "Mona/SceneViewport.h"
#include "Application/MonaApplication.h"
#include "Widgets/MViewport.h"
#include "Widgets/MWindow.h"

namespace Durin
{
	DEditorEngine* GEditor = nullptr;

	DEditorEngine::DEditorEngine(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, TransactionManager(std::make_unique<FEditorTransactionManager>())
		, NotificationManager(std::make_unique<FEditorNotificationManager>())
	{
		GEditor = this;
	}

	DEditorEngine::~DEditorEngine()
	{
		if (GEditor == this) GEditor = nullptr;
	}

	auto DEditorEngine::Init() -> void
	{
		DEngine::Init();
		EditorWorld = GetWorld();
		EditorWorld->SetWorldType(EWorldType::Editor);

		IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
		MainFrameModule.CreateDefaultMainFrame();

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
	}

	auto DEditorEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
		ReleaseRetiredPlaySessions();
		if (IsPlayingInNewWindow() && PlayWindow)
		{
			const auto& Windows = Mona::FMonaApplication::Get().GetWindows();
			if (std::ranges::find(Windows, PlayWindow) == Windows.end()) StopPlaySession();
			else SetGameInputEnabled(Mona::FMonaApplication::Get().GetActiveTopLevelWindow() == PlayWindow);
		}
		DEngine::Tick(DeltaSeconds, bIdleMode);
	}

	auto DEditorEngine::BeginDestroy() -> void
	{
		StopPlaySession();
		for (const uint64 Handle : ConsoleCommandHandles) FConsoleCommandRegistry::Get().UnregisterCommand(Handle);
		ConsoleCommandHandles.clear();
		EditorLevel = nullptr;
		EditorWorld = nullptr;
		DEngine::BeginDestroy();
		// The base teardown drains the rendering thread before releasing the scene.
		ReleaseRetiredPlaySessions(true);
	}

	auto DEditorEngine::StartPlaySession(DLevel* SourceLevel, std::string* OutError) -> bool
	{
		FEditorPlayRequest Request;
		Request.SourceLevel = SourceLevel;
		return StartPlaySession(Request, OutError);
	}

	auto DEditorEngine::StartPlaySession(const FEditorPlayRequest& Request, std::string* OutError) -> bool
	{
		DLevel* SourceLevel = Request.SourceLevel;
		if (OutError) OutError->clear();
		if (PlayState != EEditorPlayState::Stopped)
		{
			if (OutError) *OutError = "A Play session is already active.";
			return false;
		}
		if (!EditorWorld || !SourceLevel || EditorWorld->GetCurrentLevel() != SourceLevel)
		{
			if (OutError) *OutError = "The current editor level is not available for Play.";
			return false;
		}

		PlayState = EEditorPlayState::Starting;
		DWorld* NewPlayWorld = NewObject<DWorld>(this, "PlayWorld");
		NewPlayWorld->SetWorldType(EWorldType::PlayInEditor);
		std::string DuplicateError;
		EditorToPlayObjects.clear();
		DLevel* PlayLevel = Cast<DLevel>(DuplicateObjectGraph(SourceLevel, NewPlayWorld, FName(std::format("{}_PIE", SourceLevel->GetName())), &DuplicateError, &EditorToPlayObjects));
		if (!PlayLevel)
		{
			MarkObjectHierarchyAsGarbage(NewPlayWorld);
			PlayState = EEditorPlayState::Stopped;
			if (OutError) *OutError = DuplicateError.empty() ? "Could not duplicate the level for Play." : std::move(DuplicateError);
			return false;
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
			PlayState = EEditorPlayState::Stopped;
			if (OutError) *OutError = "Could not activate the duplicated Play level.";
			return false;
		}
		NewPlayWorld->SetPhysicsSimulationEnabled(Request.bSimulatePhysics);
		if (Request.StartLocation == EEditorPlayStartLocation::EditorCamera)
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
		}

		if (Request.Destination == EEditorPlayDestination::NewWindow)
		{
			PreviousSceneViewport = GetMainSceneViewport();
			PlayWindow = std::make_shared<MWindow>();
			PlayWindow->SetTitle("Durin Play");
			PlayWindow->ReshapeWindow({160.0f, 160.0f}, {1280.0f, 720.0f});
			auto ViewportWidget = std::make_shared<MViewport>();
			PlayWindow->SetContent(ViewportWidget);
			Mona::FMonaApplication::Get().AddWindow(PlayWindow, true);
			Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(PlayWindow);
			PlayWindowViewport = std::make_shared<FSceneViewport>(nullptr, PlayWindow);
			ViewportWidget->SetViewportInterface(PlayWindowViewport);
			SetMainSceneViewport(PlayWindowViewport);
			SetGameInputEnabled(true);
		}

		TransactionManager->Clear();
		NewPlayWorld->BeginPlay();
		PlayState = EEditorPlayState::Playing;
		return true;
	}

	auto DEditorEngine::StopPlaySession() -> void
	{
		if (PlayState == EEditorPlayState::Stopped) return;
		PlayState = EEditorPlayState::Stopping;
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
		SetWorld(EditorWorld.Get());
		if (EditorWorld && EditorLevel) EditorWorld->SetCurrentLevel(EditorLevel.Get(), false);
		if (PlayDestination == EEditorPlayDestination::NewWindow)
		{
			SetMainSceneViewport(PreviousSceneViewport);
			if (PlayWindow)
			{
				const auto& Windows = Mona::FMonaApplication::Get().GetWindows();
				if (std::ranges::find(Windows, PlayWindow) != Windows.end()) Mona::FMonaApplication::Get().RequestDestroyWindow(PlayWindow);
			}
			PlayWindowViewport.reset();
			PlayWindow.reset();
			PreviousSceneViewport.reset();
		}
		PlayWorld = nullptr;
		EditorLevel = nullptr;
		EditorToPlayObjects.clear();
		PlayToEditorObjects.clear();
		SetGameInputEnabled(false);
		if (WorldToDestroy)
		{
			RetiredPlayWorlds.emplace_back(WorldToDestroy);
			RetiredPlayLevels.emplace_back(LevelToDestroy);
			RetiredPlayFences.emplace_back(std::move(RetirementFence));
		}
		TransactionManager->Clear();
		PlayState = EEditorPlayState::Stopped;
		PlayDestination = EEditorPlayDestination::EmbeddedViewport;
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
		if (PlayState != EEditorPlayState::Playing && PlayState != EEditorPlayState::Paused) return;
		if (PlayWorld) PlayWorld->SetPaused(bPaused);
		PlayState = bPaused ? EEditorPlayState::Paused : EEditorPlayState::Playing;
	}

	auto DEditorEngine::StepPlaySession() -> void
	{
		if (PlayState != EEditorPlayState::Paused || !PlayWorld) return;
		PlayWorld->RequestSingleStep();
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
		for (const auto& [PlayObject, EditorObject] : PlayToEditorObjects)
		{
			if (!IsUnderSelectedActor(PlayObject)) continue;
			std::string CopyError;
			if (!CopyEditableObjectProperties(PlayObject, EditorObject, PlayToEditorObjects, &CopyError))
			{
				if (OutError) *OutError = std::move(CopyError);
				return false;
			}
		}
		for (DObject* PlayActor : SelectedRoots)
		{
			if (auto* EditorActor = Cast<AActor>(GetEditorObjectForPlayObject(PlayActor)))
			{
				if (DSceneComponent* Root = EditorActor->GetRootComponent()) Root->UpdateComponentToWorld();
				EditorActor->MarkPackageDirty();
			}
		}
		TransactionManager->Clear();
		if (OutAppliedActorCount) *OutAppliedActorCount = static_cast<uint32>(SelectedRoots.size());
		return true;
	}

	auto DEditorEngine::GetTransactionManager() -> FEditorTransactionManager&
	{
		return *TransactionManager;
	}

	auto DEditorEngine::GetNotificationManager() -> FEditorNotificationManager&
	{
		return *NotificationManager;
	}
}
