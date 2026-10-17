#include "Editor/LevelEditorSubsystem.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto Result(bool bSucceeded, std::string Message, const DEditorEngine* Editor = nullptr) -> FLevelEditorPlayResult
		{
			FLevelEditorPlayResult Value;
			Value.bSucceeded = bSucceeded;
			Value.Message = std::move(Message);
			if (Editor) Value.State = !Editor->IsPlaying() ? "stopped" : Editor->IsPlaySessionPaused() ? "paused" : "playing";
			return Value;
		}
	}

	DLevelEditorSubsystem::DLevelEditorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DLevelEditorSubsystem::Admit() const -> std::expected<DEditorEngine*, FLevelEditorPlayResult>
	{
		if (!IsInGameThread()) return std::unexpected(Result(false, "Play controls require the game thread."));
		if (!IsValid(this)) return std::unexpected(Result(false, "The level editor service is unavailable."));
		auto* Editor = GetEditor();
		const auto Gate = GetWorkGate();
		if (!IsValid(Editor) || !Gate || !Gate->IsOpen() || Editor->GetEditorSubsystem<DLevelEditorSubsystem>() != this)
			return std::unexpected(Result(false, "The level editor service is unavailable or retiring."));
		return Editor;
	}

	auto DLevelEditorSubsystem::GetPlayStatus() const -> FLevelEditorPlayResult
	{
		auto Editor = Admit(); if (!Editor) return Editor.error();
		return Result(true, !(*Editor)->IsPlaying() ? "Stopped" : (*Editor)->IsPlaySessionPaused() ? "Paused" : "Playing", *Editor);
	}

	auto DLevelEditorSubsystem::StartPlaySession() -> FLevelEditorPlayResult
	{
		auto Editor = Admit(); if (!Editor) return Editor.error();
		if (!(*Editor)->GetEditorWorld()) return Result(false, "The editor world is unavailable.", *Editor);
		std::string Error;
		if (!(*Editor)->StartPlaySession((*Editor)->GetEditorWorld()->GetCurrentLevel(), &Error))
			return Result(false, std::move(Error), *Editor);
		return Result(true, "Play session started.", *Editor);
	}

	auto DLevelEditorSubsystem::StopPlaySession() -> FLevelEditorPlayResult
	{
		auto Editor = Admit(); if (!Editor) return Editor.error();
		if (!(*Editor)->IsPlaying()) return Result(false, "No Play session is active.", *Editor);
		(*Editor)->StopPlaySession();
		return Result(true, "Play session stopped.", *Editor);
	}

	auto DLevelEditorSubsystem::SetPlaySessionPaused(bool bPaused) -> FLevelEditorPlayResult
	{
		auto Editor = Admit(); if (!Editor) return Editor.error();
		if (!(*Editor)->IsPlaying()) return Result(false, "No Play session is active.", *Editor);
		(*Editor)->SetPlaySessionPaused(bPaused);
		return Result(true, bPaused ? "Play paused." : "Play resumed.", *Editor);
	}
}
