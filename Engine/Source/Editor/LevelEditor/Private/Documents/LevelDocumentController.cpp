#include "Documents/LevelDocumentController.h"

#include "AssetSystem.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorTransaction.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Assets/EditorAssetMoveCoordinator.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Workspace/LevelEditorContext.h"
#include "Misc/Project.h"
#include "MonaImGui.h"
#include "Panels/SceneViewportPanel.h"

namespace Durin
{
	// Owns level document transitions and the unsaved-change workflow.
	FLevelDocumentController::FLevelDocumentController(
		FLevelEditorContext& InContext,
		FLevelEditorSessionSettings& InSessionSettings,
		FSceneViewportPanel& InSceneViewportPanel,
		FEditorAssetMoveCoordinator& InAssetMoveCoordinator,
		std::string& InDefaultLevel,
		std::function<void()> InClearError,
		std::function<void(std::string)> InReportError
	)
		: Context(InContext)
		, SessionSettings(InSessionSettings)
		, SceneViewportPanel(InSceneViewportPanel)
		, AssetMoveCoordinator(InAssetMoveCoordinator)
		, DefaultLevel(InDefaultLevel)
		, ClearError(std::move(InClearError))
		, ReportError(std::move(InReportError))
	{
	}

	auto FLevelDocumentController::RequestAction(ELevelDocumentAction Action) -> void
	{
		PendingLevelPath.clear();
		PendingAction = Action;
		if (Context.Level && Context.Level->GetPackage() && Context.Level->GetPackage()->IsDirty())
		{
			QueuedPopup = EQueuedPopup::UnsavedLevel;
			return;
		}
		ExecutePendingAction();
	}

	auto FLevelDocumentController::RequestOpenLevel(std::string Path) -> bool
	{
		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(Path, AssetPath)) return false;
		const Asset::FAssetData* Data = Asset::GetAssetRegistry().FindAsset(AssetPath);
		if (!Data || Data->AssetClassName != DLevel::StaticClass()->GetQualifiedName().ToString()) return false;
		PendingLevelPath = std::move(Path);
		PendingAction = ELevelDocumentAction::OpenLevel;
		if (Context.Level && Context.Level->GetPackage() && Context.Level->GetPackage()->IsDirty())
		{
			QueuedPopup = EQueuedPopup::UnsavedLevel;
			return true;
		}
		OpenLevel(PendingLevelPath);
		PendingLevelPath.clear();
		return true;
	}

	auto FLevelDocumentController::ExecutePendingAction() -> void
	{
		if (PendingAction == ELevelDocumentAction::OpenLevel)
		{
			if (!PendingLevelPath.empty())
			{
				const std::string Path = std::exchange(PendingLevelPath, {});
				OpenLevel(Path);
				return;
			}
			PendingAction = ELevelDocumentAction::None;
		}
		else if (PendingAction == ELevelDocumentAction::OpenProject)
		{
			std::string Error;
			if (!RelaunchEditorForProject({}, &Error)) SetError(std::move(Error));
		}
	}

	auto FLevelDocumentController::DrawDialogs() -> void
	{
		switch (QueuedPopup)
		{
		case EQueuedPopup::UnsavedLevel: ImGui::OpenPopup("Unsaved Level"); break;
		case EQueuedPopup::None: break;
		}
		QueuedPopup = EQueuedPopup::None;

		if (ImGui::BeginPopupModal("Unsaved Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::TextUnformatted("The current level has unsaved changes.");
			if (ImGui::Button("Save"))
			{
				if (SaveCurrentLevel())
				{
					ImGui::CloseCurrentPopup();
					ExecutePendingAction();
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Discard"))
			{
				ImGui::CloseCurrentPopup();
				ExecutePendingAction();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				PendingAction = ELevelDocumentAction::None;
				PendingLevelPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	auto FLevelDocumentController::OpenDefaultLevel() -> void
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project || DefaultLevel.empty() || !DefaultLevel.starts_with(Project->MountRoot)) return;
		OpenLevel(DefaultLevel);
	}

	auto FLevelDocumentController::OpenLevel(std::string_view PathString) -> void
	{
		if (ClearError) ClearError();
		FAssetPath Path;
		std::string PathError;
		if (!FAssetPath::TryCreate(PathString, Path, &PathError))
		{
			SetError(PathError);
			return;
		}
		DLevel* Level = nullptr;
		Asset::FAssetResult Result = Asset::LoadAsset(Path, Level);
		if (!Result)
		{
			SetError(Result.Message);
			return;
		}
		if (!ActivateLevel(Level)) return;
		PendingAction = ELevelDocumentAction::None;
	}

	auto FLevelDocumentController::SaveCurrentLevel() -> bool
	{
		if (ClearError) ClearError();
		if (!Context.Level || !Context.Level->GetPackage())
		{
			SetError("The current level is transient and cannot be saved.");
			return false;
		}
		SessionSettings.CaptureViewportState(Context, SceneViewportPanel);
		SessionSettings.Save(&SceneViewportPanel);
		Asset::FAssetResult Result = Asset::SavePackage(Context.Level->GetPackage());
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}
		return true;
	}

	auto FLevelDocumentController::RenameCurrentLevel(std::string_view NewName) -> bool
	{
		if (ClearError) ClearError();
		if (!Context.Level)
		{
			SetError("No level is open.");
			return false;
		}
		DPackage* Package = Context.Level->GetPackage();
		if (!Package || !Package->IsAssetPackage())
		{
			SetError("Transient levels cannot be renamed as assets.");
			return false;
		}
		if (NewName.empty())
		{
			SetError("Level name cannot be empty.");
			return false;
		}
		if (NewName.find_first_of("/\\") != std::string_view::npos)
		{
			SetError("Level name cannot contain path separators.");
			return false;
		}

		FAssetPath OldPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(Package->GetPackagePath(), OldPath, &PathError))
		{
			SetError(PathError);
			return false;
		}
		const std::string OldPathString = OldPath.ToString();
		const size_t Separator = OldPathString.find_last_of('/');
		const std::string NewPathString = OldPathString.substr(0, Separator + 1) + std::string(NewName);
		FAssetPath NewPath;
		if (!FAssetPath::TryCreate(NewPathString, NewPath, &PathError))
		{
			SetError(PathError);
			return false;
		}

		if (OldPath == NewPath)
		{
			if (Context.Level->GetName() == NewName) return true;
			const FName OldObjectName = Context.Level->GetFName();
			Context.Level->Rename(FName(NewName));
			const Asset::FAssetResult SaveResult = Asset::SavePackage(Package);
			if (!SaveResult)
			{
				Context.Level->Rename(OldObjectName);
				SetError(SaveResult.Message);
				return false;
			}
			return true;
		}

		const Asset::FAssetResult MoveResult = AssetMoveCoordinator.MoveAsset(OldPath, NewPath);
		if (!MoveResult)
		{
			SetError(MoveResult.Message);
			return false;
		}
		return true;
	}

	auto FLevelDocumentController::ActivateLevel(DLevel* Level) -> bool
	{
		if (!Context.World || !Level)
		{
			SetError("No world is available to activate the level.");
			return false;
		}
		SessionSettings.CaptureViewportState(Context, SceneViewportPanel);
		SessionSettings.Save(&SceneViewportPanel);
		DLevel* Previous = Context.World->GetCurrentLevel();
		DPackage* PreviousPackage = Previous ? Previous->GetPackage() : nullptr;
		if (!Context.World->SetCurrentLevel(Level))
		{
			SetError("The level is already active in another world.");
			return false;
		}
		if (GEditor) GEditor->GetTransactionManager().Clear();
		Context.Synchronize(Context.World);
		SessionSettings.RestoreViewportState(Level, SceneViewportPanel);
		if (PreviousPackage && PreviousPackage != Level->GetPackage())
		{
			FAssetPath PreviousPath;
			if (FAssetPath::TryCreate(PreviousPackage->GetPackagePath(), PreviousPath))
			{
				Asset::FAssetResult Result = Asset::UnloadPackage(PreviousPath);
				if (!Result && Result.Error != Asset::EAssetError::NotFound) DURIN_WARN("Failed to unload previous level: {}", Result.Message);
			}
		}
		return true;
	}

	auto FLevelDocumentController::SetError(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}
} // namespace Durin
