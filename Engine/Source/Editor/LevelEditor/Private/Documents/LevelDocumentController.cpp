#include "Documents/LevelDocumentController.h"
#include "Documents/LevelDocumentRevisionState.h"

#include "Asset/AssetUpgradeAuditService.h"
#include "AssetSystem.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorTransaction.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Assets/EditorAssetMoveCoordinator.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Workspace/LevelEditorContext.h"
#include "Misc/Project.h"
#include "Panels/SceneViewportPanel.h"

namespace Durin
{
	namespace
	{
		auto GetLevelTransactions() -> FEditorTransactionManager*
		{
			return GEditor ? &GEditor->GetTransactionManager() : nullptr;
		}

		auto PublishWorkspaceLoadReport(const Asset::FAssetLoadReport& Report) -> void
		{
			if (GEditor) GEditor->GetAssetUpgradeAuditService().MergeWorkspaceLoadReport(Report);
		}

		auto InvalidateAssetUpgradeReport(const FAssetPath& Path) -> void
		{
			if (GEditor) GEditor->GetAssetUpgradeAuditService().InvalidatePackage(Path);
		}

	}

	// Owns level document transitions and the unsaved-change workflow.
	FLevelDocumentController::FLevelDocumentController(
		FLevelEditorContext& InContext,
		FLevelEditorSessionSettings& InSessionSettings,
		FSceneViewportPanel& InSceneViewportPanel,
		FEditorAssetMoveCoordinator& InAssetMoveCoordinator,
		std::string& InDefaultLevel,
		std::function<void()> InClearError,
		std::function<void(std::string)> InReportError,
		std::function<void(bool)> InCompleteDeferredOpen
	)
		: Context(InContext)
		, SessionSettings(InSessionSettings)
		, SceneViewportPanel(InSceneViewportPanel)
		, AssetMoveCoordinator(InAssetMoveCoordinator)
		, DefaultLevel(InDefaultLevel)
		, ClearError(std::move(InClearError))
		, ReportError(std::move(InReportError))
		, CompleteDeferredOpen(std::move(InCompleteDeferredOpen))
	{
	}

	auto FLevelDocumentController::RequestAction(ELevelDocumentAction Action) -> void
	{
		if (PendingUpgrade.IsPending()) return;
		PendingLevelPath.clear();
		bPendingDocumentOpen = false;
		PendingAction = Action;
		if (Context.Level && Context.Level->GetPackage() && Context.Level->GetPackage()->IsDirty())
		{
			QueuedPopup = EQueuedPopup::UnsavedLevel;
			return;
		}
		ExecutePendingAction();
	}

	auto FLevelDocumentController::RequestOpenLevel(std::string Path) -> ELevelDocumentOpenResult
	{
		if (PendingUpgrade.IsPending()) return ELevelDocumentOpenResult::Rejected;
		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(Path, AssetPath)) return ELevelDocumentOpenResult::Rejected;
		const Asset::FAssetData* Data = Asset::GetAssetRegistry().FindAsset(AssetPath);
		if (!Data || Data->AssetClassName != DLevel::StaticClass()->GetQualifiedName().ToString())
			return ELevelDocumentOpenResult::Rejected;
		PendingLevelPath = std::move(Path);
		PendingAction = ELevelDocumentAction::OpenLevel;
		bPendingDocumentOpen = true;
		if (Context.Level && Context.Level->GetPackage() && Context.Level->GetPackage()->IsDirty())
		{
			QueuedPopup = EQueuedPopup::UnsavedLevel;
			return ELevelDocumentOpenResult::Deferred;
		}
		const ELevelDocumentOpenResult Result = OpenLevel(PendingLevelPath);
		if (Result != ELevelDocumentOpenResult::Deferred)
		{
			PendingLevelPath.clear();
			PendingAction = ELevelDocumentAction::None;
			bPendingDocumentOpen = false;
		}
		return Result;
	}

	auto FLevelDocumentController::ExecutePendingAction() -> ELevelDocumentOpenResult
	{
		if (PendingAction == ELevelDocumentAction::OpenLevel)
		{
			if (!PendingLevelPath.empty())
			{
				const ELevelDocumentOpenResult Result = OpenLevel(PendingLevelPath);
				if (Result != ELevelDocumentOpenResult::Deferred)
				{
					PendingLevelPath.clear();
					PendingAction = ELevelDocumentAction::None;
				}
				return Result;
			}
			PendingAction = ELevelDocumentAction::None;
			return ELevelDocumentOpenResult::Rejected;
		}
		else if (PendingAction == ELevelDocumentAction::OpenProject)
		{
			std::string Error;
			if (!RelaunchEditorForProject({}, &Error))
			{
				SetError(std::move(Error));
				return ELevelDocumentOpenResult::Rejected;
			}
			return ELevelDocumentOpenResult::Opened;
		}
		return ELevelDocumentOpenResult::Rejected;
	}

	auto FLevelDocumentController::DrawDialogs() -> void
	{
		const bool bOpenUnsavedLevel = QueuedPopup == EQueuedPopup::UnsavedLevel;
		QueuedPopup = EQueuedPopup::None;
		// Capture the pending state before resolving an unsaved level so a newly loaded
		// compatibility report opens on the following frame, matching popup scheduling.
		const bool bOpenAssetStructureUpgrade = PendingUpgrade.IsPending();
		(void)UnsavedLevelDialog.Draw(
			bOpenUnsavedLevel,
			[this](EUnsavedLevelDialogDecision Decision) {
				return ResolveUnsavedLevelDialog(Decision);
			});
		(void)AssetStructureUpgradeDialog.Draw(
			PendingUpgrade,
			bOpenAssetStructureUpgrade,
			bCompatibilityDataLossConfirmed,
			[this](EAssetStructureUpgradeDecision Decision) {
				return ResolvePendingLevelUpgrade(Decision);
			});
	}

	auto FLevelDocumentController::ResolveUnsavedLevelDialog(EUnsavedLevelDialogDecision Decision) -> bool
	{
		if (Decision == EUnsavedLevelDialogDecision::None) return false;
		if (Decision == EUnsavedLevelDialogDecision::Save && !SaveCurrentLevel()) return false;
		if (Decision == EUnsavedLevelDialogDecision::Cancel)
		{
			const bool bCancelsDeferredOpen = PendingAction == ELevelDocumentAction::OpenLevel;
			PendingAction = ELevelDocumentAction::None;
			PendingLevelPath.clear();
			if (bCancelsDeferredOpen) CompletePendingDocumentOpen(false);
			return true;
		}
		if (Decision != EUnsavedLevelDialogDecision::Save
			&& Decision != EUnsavedLevelDialogDecision::Discard)
			return false;
		const bool bCompletesDeferredOpen = PendingAction == ELevelDocumentAction::OpenLevel;
		const ELevelDocumentOpenResult Result = ExecutePendingAction();
		if (bCompletesDeferredOpen && Result != ELevelDocumentOpenResult::Deferred)
			CompletePendingDocumentOpen(Result == ELevelDocumentOpenResult::Opened);
		return true;
	}

	auto FLevelDocumentController::OpenDefaultLevel() -> void
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project || DefaultLevel.empty() || !DefaultLevel.starts_with(Project->MountRoot)) return;
		OpenLevel(DefaultLevel);
	}

	auto FLevelDocumentController::OpenLevel(std::string_view PathString) -> ELevelDocumentOpenResult
	{
		if (ClearError) ClearError();
		FAssetPath Path;
		std::string PathError;
		if (!FAssetPath::TryCreate(PathString, Path, &PathError))
		{
			SetError(PathError);
			return ELevelDocumentOpenResult::Rejected;
		}
		DLevel* Level = nullptr;
		Asset::FAssetLoadReport LoadReport;
		Asset::FAssetResult Result = Asset::LoadAsset(Path, Level, &LoadReport);
		if (!Result)
		{
			SetError(Result.Message);
			return ELevelDocumentOpenResult::Rejected;
		}
		PublishWorkspaceLoadReport(LoadReport);
		if (LoadReport.HasCompatibilityIssues())
		{
			const bool bCompletesDeferredOpen = std::exchange(bPendingDocumentOpen, false);
			PendingUpgrade.Begin(Level, std::move(LoadReport), bCompletesDeferredOpen);
			bCompatibilityDataLossConfirmed = false;
			return ELevelDocumentOpenResult::Deferred;
		}
		if (!ActivateLevel(Level))
		{
			Asset::FAssetResult UnloadResult = Asset::UnloadPackage(Path);
			if (!UnloadResult && UnloadResult.Error != Asset::EAssetError::NotFound)
				DURIN_WARN("Failed to unload level after activation failed: {}", UnloadResult.Message);
			return ELevelDocumentOpenResult::Rejected;
		}
		PendingAction = ELevelDocumentAction::None;
		return ELevelDocumentOpenResult::Opened;
	}

	auto FLevelDocumentController::ResolvePendingLevelUpgrade(
		EAssetStructureUpgradeDecision Decision) -> EAssetStructureUpgradeResult
	{
		FAssetStructureUpgradeOperations Operations{
			.Save = [this](DLevel* Level, bool bAllowCompatibilityDataLoss) {
				if (!Level || !Level->GetPackage())
				return SetError("The pending level is no longer available."), false;
				Asset::FAssetResult Result = Asset::SavePackage(
					Level->GetPackage(),
					{.bAllowCompatibilityDataLoss = bAllowCompatibilityDataLoss});
				if (Result)
				{
					InvalidateAssetUpgradeReport(PendingUpgrade.GetReport().PackagePath);
					return true;
				}
				SetError(Result.Message);
				return false;
			},
			.Activate = [this](DLevel* Level) { return ActivateLevel(Level); },
			.Unload = [this](const FAssetPath& Path) {
				Asset::FAssetResult Result = Asset::UnloadPackage(Path);
				if (!Result && Result.Error != Asset::EAssetError::NotFound)
					SetError(std::format("Could not unload the pending level: {}", Result.Message));
			},
			.CompleteDeferredOpen = [this](bool bSucceeded) {
				if (CompleteDeferredOpen) CompleteDeferredOpen(bSucceeded);
			}};
		const EAssetStructureUpgradeResult Result = PendingUpgrade.Resolve(Decision, Operations);
		if (Result != EAssetStructureUpgradeResult::SaveFailed)
		{
			PendingAction = ELevelDocumentAction::None;
			PendingLevelPath.clear();
			bCompatibilityDataLossConfirmed = false;
		}
		return Result;
	}

	auto FLevelDocumentController::CompletePendingDocumentOpen(bool bSucceeded) -> void
	{
		if (!std::exchange(bPendingDocumentOpen, false) || !CompleteDeferredOpen) return;
		CompleteDeferredOpen(bSucceeded);
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
		FLevelDocumentRevisionState::CompleteSave(
			GetLevelTransactions(), *Context.Level->GetPackage(), static_cast<bool>(Result)
		);
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}
		FAssetPath SavedPath;
		if (FAssetPath::TryCreate(Context.Level->GetPackage()->GetPackagePath(), SavedPath))
			InvalidateAssetUpgradeReport(SavedPath);
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
			const bool bWasDirty = Package->IsDirty();
			Context.Level->Rename(FName(NewName));
			const Asset::FAssetResult SaveResult = Asset::SavePackage(Package);
			FLevelDocumentRevisionState::CompleteSave(
				GetLevelTransactions(), *Package, static_cast<bool>(SaveResult)
			);
			if (!SaveResult)
			{
				Context.Level->Rename(OldObjectName);
				if (!bWasDirty) Package->ClearDirty();
				SetError(SaveResult.Message);
				return false;
			}
			InvalidateAssetUpgradeReport(OldPath);
			return true;
		}

		const Asset::FAssetResult MoveResult = AssetMoveCoordinator.MoveAsset(OldPath, NewPath);
		FLevelDocumentRevisionState::CompleteSave(
			GetLevelTransactions(), *Package, static_cast<bool>(MoveResult)
		);
		if (!MoveResult)
		{
			SetError(MoveResult.Message);
			return false;
		}
		InvalidateAssetUpgradeReport(OldPath);
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
		FLevelDocumentRevisionState::Activate(GetLevelTransactions(), Level->GetPackage());
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
