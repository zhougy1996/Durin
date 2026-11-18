#include "LevelEditorSubsystem.h"
#include "FileHelpers.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	DLevelEditorSubsystem::DLevelEditorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DLevelEditorSubsystem::Initialize(FSubsystemCollection& Collection) -> void
	{ Collection.InitializeDependency<DAssetEditorSubsystem>(); }

	auto DLevelEditorSubsystem::LoadLevel(const std::string& AssetPath) -> bool
	{
		if (!GetActiveEditor()) return false;
		return DEditorLoadingAndSavingUtils::LoadMap(AssetPath) != nullptr;
	}

	auto DLevelEditorSubsystem::Deinitialize() noexcept -> void
	{
		HostContext.reset();
	}

	auto DLevelEditorSubsystem::GetActiveEditor() const -> DEditorEngine*
	{
		if (!IsInGameThread() || !IsValid(this)) return nullptr;
		auto* Editor = GetEditor();
		return IsValid(Editor) && !Editor->IsShutdownRequested()
			&& Editor->GetEditorSubsystem<DLevelEditorSubsystem>() == this ? Editor : nullptr;
	}

	auto DLevelEditorSubsystem::IsInPlayInEditor() const -> bool
	{
		const auto* Editor = GetActiveEditor();
		return Editor && Editor->IsPlaying();
	}

	auto DLevelEditorSubsystem::EditorRequestBeginPlay() -> void
	{
		auto* Editor = GetActiveEditor();
		if (!Editor || Editor->IsPlaying()) return;
		if (!Editor->GetEditorWorld())
		{ DURIN_WARN("Cannot begin PIE: the editor world is unavailable."); return; }
		std::string Error;
		if (!Editor->StartPlaySession(Editor->GetEditorWorld()->GetCurrentLevel(), &Error))
			DURIN_WARN("Cannot begin PIE: {}", Error);
	}

	auto DLevelEditorSubsystem::EditorRequestEndPlay() -> void
	{
		if (auto* Editor = GetActiveEditor()) Editor->StopPlaySession();
	}

	auto DLevelEditorSubsystem::SetPlaySessionPaused(bool bPaused) -> bool
	{
		auto* Editor = GetActiveEditor();
		if (!Editor || !Editor->IsPlaying()) return false;
		Editor->SetPlaySessionPaused(bPaused);
		return Editor->IsPlaySessionPaused() == bPaused;
	}

	auto DLevelEditorSubsystem::IsPlaySessionPaused() const -> bool
	{
		const auto* Editor = GetActiveEditor();
		return Editor && Editor->IsPlaying() && Editor->IsPlaySessionPaused();
	}
}
