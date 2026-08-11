#include "LaunchGameplayValidation.h"

#include "Actors/GameMode.h"
#include "Actors/PlayerStart.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineGlobals.h"
#include "Mona.h"
#include "Widgets/MWindow.h"
#include "Window/GenericWindow.h"

#if DURIN_WITH_EDITOR
	#include "Editor/EditorEngine.h"
#endif

namespace Durin
{
#if DURIN_WITH_EDITOR
	auto TryRunEditorPIELifecycleSmoke() -> bool
	{
		if (!GEditor || !GEditor->GetEditorWorld()) return false;
		DWorld* EditorWorld = GEditor->GetEditorWorld();
		DLevel* SourceLevel = EditorWorld->GetCurrentLevel();
		if (!SourceLevel) return false;

		for (const EEditorPlayDestination Destination : {
			EEditorPlayDestination::EmbeddedViewport,
			EEditorPlayDestination::NewWindow})
		{
			for (const EEditorPlayStartLocation StartLocation : {
				EEditorPlayStartLocation::LevelStart,
				EEditorPlayStartLocation::EditorCamera})
			{
				FEditorPlayRequest Request;
				Request.SourceLevel = SourceLevel;
				Request.Destination = Destination;
				Request.StartLocation = StartLocation;
				Request.CameraLocation = {11.0, 12.0, 13.0};
				Request.CameraTarget = {12.0, 12.0, 13.0};
				std::string Error;
				const bool bStarted = GEditor->StartPlaySession(Request, &Error);
				checkf(bStarted, "PIE lifecycle smoke could not start: {}", Error);
				if (!bStarted) return true;
				checkf(GEditor->GetPlayState() == EEditorPlayState::Playing
					&& GEditor->GetPlayWorld()
					&& GEditor->IsPlayingInNewWindow()
						== (Destination == EEditorPlayDestination::NewWindow),
					"PIE lifecycle smoke published an incoherent host state.");
				if (StartLocation == EEditorPlayStartLocation::EditorCamera)
				{
					checkf(GEditor->GetPlayWorld()->FindActorByName("PIE_EditorCamera"),
						"Play From Camera did not publish its transient camera.");
				}
				const std::shared_ptr<MWindow> ActiveWindow = Mona::FMonaApplication::Get().GetActiveTopLevelWindow();
				const std::shared_ptr<FGenericWindow> NativeWindow = ActiveWindow ? ActiveWindow->GetNativeWindow() : nullptr;
				checkf(NativeWindow, "PIE mouse-capture smoke could not resolve the active native window.");
				if (NativeWindow)
				{
					NativeWindow->Focus();
					if (Destination == EEditorPlayDestination::EmbeddedViewport)
						GEditor->UpdateEmbeddedPlayMouseTarget(NativeWindow, true, false);
					for (int32 Cycle = 0; Cycle < 10; ++Cycle)
					{
						const bool bCaptured = GEditor->RequestPlayMouseCapture(NativeWindow);
						checkf(bCaptured
							&& GEditor->IsPlayMouseCaptured()
							&& NativeWindow->GetCursorMode() == ECursorMode::Captured,
							"PIE mouse-capture smoke failed to capture on cycle {}.", Cycle);
						GEditor->ReleasePlayMouseCapture();
						checkf(!GEditor->IsPlayMouseCaptured()
							&& NativeWindow->GetCursorMode() == ECursorMode::Free,
							"PIE mouse-capture smoke failed to release on cycle {}.", Cycle);
					}
				}
				GEditor->SetPlaySessionPaused(true);
				GEditor->StepPlaySession();
				GEditor->Tick(1.0f / 60.0f, false);
				GEditor->StopPlaySession();
				checkf(GEditor->GetPlayState() == EEditorPlayState::Stopped
					&& GEditor->GetWorld() == EditorWorld
					&& EditorWorld->GetCurrentLevel() == SourceLevel,
					"PIE lifecycle smoke did not restore the editor World.");
				Mona::FMonaApplication::Get().Tick();
			}
		}
		DURIN_INFO("Editor PIE lifecycle smoke passed all four host/start combinations and repeated mouse capture/release.");
		return true;
	}
#endif

	auto RunNativeGameplayLifecycleSmoke() -> void
	{
		DWorld* OriginalWorld = GEngine ? GEngine->GetWorld() : nullptr;
		checkf(GEngine && OriginalWorld,
			"Native gameplay lifecycle smoke requires an initialized Engine World.");
		if (!GEngine || !OriginalWorld) return;

		auto* SmokeWorld = NewObject<DWorld>(GEngine, "NativeGameplayLifecycleSmokeWorld");
		auto* SmokeLevel = NewObject<DLevel>(SmokeWorld, "NativeGameplayLifecycleSmokeLevel");
		GEngine->SetWorld(SmokeWorld);
		const auto Restore = [&] {
			SmokeWorld->EndPlay();
			GEngine->SetWorld(OriginalWorld);
			MarkObjectHierarchyAsGarbage(SmokeWorld);
		};
		if (!SmokeWorld->SetCurrentLevel(SmokeLevel)
			|| !SmokeLevel->SpawnActor<APlayerStart>("Start"))
		{
			Restore();
			checkf(false, "Native gameplay lifecycle smoke could not prepare its World.");
			return;
		}

		const FWorldPlayResult PlayResult = SmokeWorld->BeginPlay({
			.GameModeClass = AGameMode::StaticClass()});
		if (!PlayResult
			|| !SmokeWorld->GetGameMode()
			|| !SmokeWorld->GetLocalPlayerController()
			|| !SmokeWorld->GetDefaultPawn())
		{
			const std::string Error = PlayResult.Message;
			Restore();
			checkf(false, "Native gameplay lifecycle smoke could not start: {}", Error);
			return;
		}
		SmokeWorld->Tick({
			.DeltaSeconds = 1.0f / 60.0f,
			.GameInput = &GEngine->GetGameInputState()});
		SmokeWorld->SetPaused(true);
		SmokeWorld->Tick({
			.DeltaSeconds = 1.0f / 60.0f,
			.GameInput = &GEngine->GetGameInputState()});
		SmokeWorld->RequestSingleStep();
		SmokeWorld->Tick({
			.DeltaSeconds = 1.0f / 60.0f,
			.GameInput = &GEngine->GetGameInputState()});
		SmokeWorld->SetPaused(false);
		const FPlayerRestartResult Restart = SmokeWorld->RestartPlayer();
		if (!Restart)
		{
			const std::string Error = Restart.Message;
			Restore();
			checkf(false, "Native gameplay lifecycle smoke could not restart: {}", Error);
			return;
		}
		Restore();
		DURIN_INFO("Native gameplay lifecycle smoke completed start, tick, step, restart, and stop.");
	}
}
