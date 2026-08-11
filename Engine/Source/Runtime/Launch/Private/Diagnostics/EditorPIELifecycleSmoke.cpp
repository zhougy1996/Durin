#include "EditorPIELifecycleSmoke.h"

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

		for (const Editor::EPlayDestination Destination : {
			Editor::EPlayDestination::EmbeddedViewport,
			Editor::EPlayDestination::NewWindow})
		{
			for (const Editor::EPlayStartLocation StartLocation : {
				Editor::EPlayStartLocation::LevelStart,
				Editor::EPlayStartLocation::EditorCamera})
			{
				Editor::FPlayRequest Request;
				Request.SourceLevel = SourceLevel;
				Request.Destination = Destination;
				Request.StartLocation = StartLocation;
				Request.CameraLocation = {11.0, 12.0, 13.0};
				Request.CameraTarget = {12.0, 12.0, 13.0};
				std::string Error;
				const bool bStarted = GEditor->StartPlaySession(Request, &Error);
				checkf(bStarted, "PIE lifecycle smoke could not start: {}", Error);
				if (!bStarted) return true;
				checkf(GEditor->GetPlayState() == Editor::EPlayState::Playing
					&& GEditor->GetPlayWorld()
					&& GEditor->IsPlayingInNewWindow()
						== (Destination == Editor::EPlayDestination::NewWindow),
					"PIE lifecycle smoke published an incoherent host state.");
				if (StartLocation == Editor::EPlayStartLocation::EditorCamera)
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
					if (Destination == Editor::EPlayDestination::EmbeddedViewport)
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
				checkf(GEditor->GetPlayState() == Editor::EPlayState::Stopped
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
}
