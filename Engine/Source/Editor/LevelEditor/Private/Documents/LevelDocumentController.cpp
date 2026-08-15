#include "Documents/LevelDocumentController.h"
#include "Documents/LevelDocumentRevisionState.h"

#include "Animation/AnimationClip.h"
#include "MultiOutputImport.h"
#include "ImportService.h"
#include "SceneImport.h"
#include "AssetMutation.h"
#include "Editor/EditorEngine.h"
#include "Editor/Transaction.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Assets/EditorAssetMoveCoordinator.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Workspace/LevelEditorContext.h"
#include "Misc/Project.h"
#include "Panels/SceneViewportPanel.h"
#include "Profiling/Profiling.h"
#include "SkeletalMesh/SkeletalDerivedData.h"
#include "SkeletalMesh/SkeletalMesh.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto GetLevelTransactions() -> ::Durin::Editor::FTransactionManager*
		{
			return GEditor ? &GEditor->GetTransactionManager() : nullptr;
		}

		auto RepairMissingSkeletalDerivedData(
			std::span<DObject* const> MissingAssets,
			std::string& OutError) -> bool
		{
			const std::vector<DObject*> Assets(MissingAssets.begin(), MissingAssets.end());
			std::vector<Asset::Import::DImportRecord*> Records;
			for (DObject* Asset : Assets)
			{
				std::string RecordError;
				Asset::Import::DImportRecord* Record = Asset::Import::Standard::FindSceneImportRecordForOutput(
					*Asset, RecordError);
				if (!Record)
				{
					OutError = std::format(
						"Could not rebuild missing skeletal derived data for '{}': {}",
						Asset->GetObjectPath(), RecordError);
					return false;
				}
				if (std::ranges::find(Records, Record) == Records.end())
					Records.push_back(Record);
			}

			for (Asset::Import::DImportRecord* Record : Records)
			{
				const Asset::Import::FImportRecordActionResult Result =
					Asset::Import::GetImportService().ExecuteImportRecordAction(
						*Record,
						Asset::Import::EImportRecordAction::Reimport);
				if (!Result)
				{
					OutError = std::format(
						"Could not rebuild skeletal derived data from its Scene import record: {}",
						Result.Message);
					return false;
				}
			}

			for (DObject* Asset : Assets)
			{
				const DAnimationClip* Clip = Cast<DAnimationClip>(Asset);
				const DSkeletalMesh* Mesh = Cast<DSkeletalMesh>(Asset);
				const bool bReady = Clip ? Clip->GetPayloadData() != nullptr
					: Mesh && Mesh->GetPayloadData() != nullptr;
				if (!bReady)
				{
					OutError = std::format(
						"Scene reimport did not restore skeletal derived data for '{}'.",
						Asset->GetObjectPath());
					return false;
				}
			}
			if (!Assets.empty())
				DURIN_INFO("Rebuilt missing skeletal derived data for {} asset(s) before opening the level.",
					Assets.size());
			OutError.clear();
			return true;
		}

	}

	// Owns level document transitions and the unsaved-change workflow.
	FLevelDocumentController::FLevelDocumentController(
		FLevelEditorContext& InContext,
		FLevelEditorSessionSettings& InSessionSettings,
		FSceneViewportPanel& InSceneViewportPanel,
		FEditorAssetMoveCoordinator& InAssetMoveCoordinator,
		TSoftObjectPtr<DLevel>& InDefaultLevel,
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
		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(Path, AssetPath)) return ELevelDocumentOpenResult::Rejected;
		const Asset::FAssetPathResolveResult Resolution =
			Asset::ResolveAssetPath(
				AssetPath, {.ExpectedClass = DLevel::StaticClass()});
		if (!Resolution)
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
		(void)UnsavedLevelDialog.Draw(
			bOpenUnsavedLevel,
			[this](EUnsavedLevelDialogDecision Decision) {
				return ResolveUnsavedLevelDialog(Decision);
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

	auto FLevelDocumentController::OpenDefaultLevel() -> bool
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project || DefaultLevel.IsNull()
			|| !DefaultLevel.GetSoftObjectPath().GetView().starts_with(Project->MountRoot))
			return true;
		if (ClearError) ClearError();
		const FAssetPath& Path = DefaultLevel.GetSoftObjectPath().GetAssetPath();
		const Asset::FAssetPackageLoadSnapshot LoadSnapshot =
			Asset::CapturePackageLoadSnapshot();
		DLevel* Level = nullptr;
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultDocumentAssetLoadBegin);
		Asset::FAssetResult Result;
		std::string DerivedDataRepairError;
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.DefaultDocument.AssetLoad");
			const FScopedSkeletalDerivedDataRepairLoad RepairLoad;
			Result = Asset::LoadSoftObject(
				DefaultLevel, Level, Asset::ESoftObjectNullPolicy::Reject);
			if (Result && !RepairMissingSkeletalDerivedData(
				RepairLoad.GetMissingAssets(), DerivedDataRepairError))
				Result = {Asset::EAssetError::InvalidObjectGraph, DerivedDataRepairError};
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultDocumentAssetLoadComplete);
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultDocumentActivationBegin);
		bool bActivated = false;
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.DefaultDocument.Activation");
			bActivated = ActivateLevel(Level);
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultDocumentActivationComplete);
		if (!bActivated)
		{
			const Asset::FAssetResult ReleaseResult =
				Asset::ReleasePackagesLoadedSince(LoadSnapshot);
			if (!ReleaseResult)
				DURIN_WARN(
					"Failed to release packages after default-level activation failed: {}",
					ReleaseResult.Message);
			return false;
		}
		return true;
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
		const Asset::FAssetPackageLoadSnapshot LoadSnapshot =
			Asset::CapturePackageLoadSnapshot();
		DLevel* Level = nullptr;
		Asset::FAssetResult Result;
		std::string DerivedDataRepairError;
		{
			const FScopedSkeletalDerivedDataRepairLoad RepairLoad;
			Result = Asset::LoadAsset(Path, Level);
			if (Result && !RepairMissingSkeletalDerivedData(
				RepairLoad.GetMissingAssets(), DerivedDataRepairError))
				Result = {Asset::EAssetError::InvalidObjectGraph, DerivedDataRepairError};
		}
		if (!Result)
		{
			SetError(Result.Message);
			return ELevelDocumentOpenResult::Rejected;
		}
		if (!ActivateLevel(Level))
		{
			const Asset::FAssetResult ReleaseResult =
				Asset::ReleasePackagesLoadedSince(LoadSnapshot);
			if (!ReleaseResult)
				DURIN_WARN("Failed to release packages after level activation failed: {}", ReleaseResult.Message);
			return ELevelDocumentOpenResult::Rejected;
		}
		PendingAction = ELevelDocumentAction::None;
		return ELevelDocumentOpenResult::Opened;
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
			return true;
		}

		const FEditorAssetMove Move{OldPath, NewPath};
		const Asset::FAssetResult MoveResult =
			AssetMoveCoordinator.MoveAssets(std::span{&Move, 1});
		FLevelDocumentRevisionState::CompleteSave(
			GetLevelTransactions(), *Package, static_cast<bool>(MoveResult)
		);
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
} // namespace Durin::Editor::Level
