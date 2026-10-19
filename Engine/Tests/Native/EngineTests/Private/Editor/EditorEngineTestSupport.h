#pragma once

#include "CoreMinimal.h"
#include "Editor/EditorEngine.h"

namespace Durin
{
	// Shared native-test access; production editor startup remains explicit.
	struct FEditorEngineTestAccess
	{
		static auto InitializeServices(DEditorEngine& Editor) -> FSubsystemResult
		{ return Editor.InitializeEditorSubsystems(); }

		static auto StartEditor(DEditorEngine& Editor) -> FSubsystemResult
		{
			if (!Editor.ActivateEditor())
				return {ESubsystemError::InvalidState, "Editor activation failed."};
			return InitializeServices(Editor);
		}

		static auto SetEditorWorld(DEditorEngine& Editor, DWorld* World) -> void
		{ Editor.EditorWorld = World; Editor.SetWorld(World); }

		static auto SetState(DEditorEngine& Editor, Editor::EPlayState State) -> void
		{ Editor.PlayState = State; }

		static auto StartPlaySession(
			DEditorEngine& Engine,
			const Editor::FPlayRequest& Request,
			DClass* GameModeClass,
			std::string* OutError = nullptr) -> bool
		{
			return Engine.StartPlaySessionInternal(
				Request, std::optional<DClass*>{GameModeClass}, OutError);
		}

		static auto ConfigureMouseCapture(
			DEditorEngine& Engine,
			Editor::EPlayDestination Destination = Editor::EPlayDestination::EmbeddedViewport) -> void
		{
			Engine.PlayState = Editor::EPlayState::Playing;
			Engine.PlayDestination = Destination;
		}

		static auto InitializePlayWindowViewportClient(
			DEditorEngine& Engine,
			const FViewportClient* SourceClient) -> void
		{
			Engine.InitializePlayWindowViewportClient(SourceClient);
		}

		static auto KeyDown(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, EKey Key, bool bRepeat = false) -> bool
		{
			return Engine.HandleGameInputKeyDown(Window, Key, bRepeat);
		}

		static auto Focus(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, bool bFocused) -> void
		{
			Engine.HandleGameInputWindowFocus(Window, bFocused);
		}

		static auto MouseDown(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, EMouseButton Button) -> bool
		{
			return Engine.HandleGameInputMouseDown(Window, Button);
		}

		static auto Close(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window) -> void
		{
			Engine.HandleGameInputWindowClose(Window);
		}
	};
}
