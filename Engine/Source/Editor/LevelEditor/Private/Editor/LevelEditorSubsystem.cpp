#include "Editor/LevelEditorSubsystem.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "LevelEditorModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Widgets/MLevelEditor.h"
#include "Documents/EditorDocumentManager.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	DLevelEditorSubsystem::DLevelEditorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DLevelEditorSubsystem::Initialize(FSubsystemCollection& Collection) -> void
	{ Collection.InitializeDependency<DAssetEditorSubsystem>(); }

	auto DLevelEditorSubsystem::LoadLevel(const std::string& AssetPath) -> bool
	{
		if (!GetActiveEditor()) return false;
		auto* Assets = GetEditor()->GetEditorSubsystem<DAssetEditorSubsystem>();
		if (!Assets) return false;
		auto* Owner = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor").GetOrCreateLevelEditor(*Assets);
		if (!Owner) return false;
		const auto Toolkit = Owner->GetToolkit();
		const auto Documents = Toolkit->GetDocumentManager();
		return Documents->OpenDocument({.ToolkitId = Toolkit->GetToolkitId(), .DocumentKey = "LevelEditor",
			.ResourceId = AssetPath, .Label = "Level Editor"}).IsValid();
	}
	auto DLevelEditorSubsystem::OpenLevel(const std::string& AssetPath) -> bool
	{
		if (!GetActiveEditor()) return false;
		auto* Assets = GetEditor()->GetEditorSubsystem<DAssetEditorSubsystem>();
		if (!Assets) return false;
		auto* Owner = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor").GetOrCreateLevelEditor(*Assets);
		const auto Toolkit = Owner ? std::dynamic_pointer_cast<LevelEditor::MLevelEditor>(Owner->GetToolkit()) : nullptr;
		return Toolkit && Toolkit->OpenLevel(AssetPath);
	}
	auto DLevelEditorSubsystem::CancelLevelLoad() -> void
	{
		CheckGameThread();
		if (auto* Assets = GetEditor()->GetEditorSubsystem<DAssetEditorSubsystem>())
			for (const auto& Toolkit : Assets->GetAssetToolkits())
				if (const auto Level = std::dynamic_pointer_cast<LevelEditor::MLevelEditor>(Toolkit)) (void)Level->CancelPendingOpen();
	}
	auto DLevelEditorSubsystem::Deinitialize() noexcept -> void
	{
		CancelLevelLoad();
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
