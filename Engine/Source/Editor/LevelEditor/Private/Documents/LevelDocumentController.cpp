#include "Asset/RegistryOperations.h"
#include "Documents/LevelDocumentController.h"
#include "Documents/LevelDocumentRevisionState.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/Asset.h"
#include "Editor/EditorEngine.h"
#include "Transactions/Transaction.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Assets/EditorAssetMoveCoordinator.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Engine/SceneLoadOperation.h"
#include "Workspace/LevelEditorContext.h"
#include "Misc/Project.h"
#include "Panels/SceneViewportPanel.h"
#include "Viewport/TransformGizmo.h"
#include "Profiling/Profiling.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto GetLevelTransactions() -> ::Durin::DTransactor*
		{
			return GEditor ? GEditor->GetTransactor() : nullptr;
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

	auto FLevelDocumentController::RequestOpenProject(std::string ProjectFile) -> void
	{
		PendingLevelPath.clear();
		bPendingDocumentOpen = false;
		PendingProjectFile = std::move(ProjectFile);
		PendingAction = ELevelDocumentAction::OpenProject;
		if (Context.Level && Context.Level->GetPackage() && Context.Level->GetPackage()->IsDirty())
		{
			QueuedPopup = EQueuedPopup::UnsavedLevel;
			return;
		}
		ExecutePendingAction();
	}

	auto FLevelDocumentController::RequestOpenLevel(std::string Path) -> ELevelDocumentOpenResult
	{
		PendingProjectFile.clear();
		FObjectPath AssetPath;
		if (!FObjectPath::TryCreate(Path, AssetPath)) return ELevelDocumentOpenResult::Rejected;
		const FObjectPathResolveResult Resolution =
			ResolveAssetObjectPathForOperation(
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
			const auto RelaunchEditorForProjectResult = RelaunchEditorForProject(PendingProjectFile);
			Error = RelaunchEditorForProjectResult ? std::string{} : RelaunchEditorForProjectResult.error().ToString();
			if (!RelaunchEditorForProjectResult.has_value())
			{
				SetError(std::move(Error));
				return ELevelDocumentOpenResult::Rejected;
			}
			return ELevelDocumentOpenResult::Opened;
		}
		return ELevelDocumentOpenResult::Rejected;
	}

	auto FLevelDocumentController::AdvancePendingOpen() -> void
	{
		if (PendingSceneLoad && (!bDefaultStarted || DefaultOpenState != EHostStartupState::Pending))
			(void)AdvanceOpening();
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
			PendingProjectFile.clear();
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

	auto FLevelDocumentController::OpenDefaultLevel() -> EHostStartupState
	{
		if (!bDefaultStarted)
		{
			bDefaultStarted = true;
			const FProjectInfo* Project = GetCurrentProject();
			if (!Project || DefaultLevel.IsNull()) return DefaultOpenState = EHostStartupState::Ready;
			if (!DefaultLevel.GetPath().GetPackagePath().GetView().starts_with(Project->MountRoot))
			{
				SetError("The configured default Level is outside the selected project mount.");
				return DefaultOpenState = EHostStartupState::Failed;
			}
			Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultDocumentAssetLoadBegin);
			if (OpenLevel(DefaultLevel.GetPath().ToString()) == ELevelDocumentOpenResult::Rejected)
				return DefaultOpenState = EHostStartupState::Failed;
		}
		if (DefaultOpenState != EHostStartupState::Pending) return DefaultOpenState;
		DefaultOpenState = AdvanceOpening();
		return DefaultOpenState;
	}

	auto FLevelDocumentController::OpenLevel(std::string_view PathString) -> ELevelDocumentOpenResult
	{
		if (ClearError) ClearError();
		if (!Context.EditorHost || Context.EditorHost->IsPlaying()) return ELevelDocumentOpenResult::Rejected;
		FObjectPath Path;
		if (const auto Validation = FObjectPath::TryCreateWithDiagnostic(PathString, Path); !Validation)
		{
			SetError(Durin::ToString(Validation.error()));
			return ELevelDocumentOpenResult::Rejected;
		}
		if (PendingSceneLoad) PendingSceneLoad->Cancel();
		PendingSceneLoad = Context.EditorHost->CreateSceneLoad({.Path = Path, .BuildInitialView = SessionSettings.CreateInitialViewBuilder(SceneViewportPanel)});
		return ELevelDocumentOpenResult::Deferred;
	}

	auto FLevelDocumentController::AdvanceOpening() -> EHostStartupState
	{
		if (!PendingSceneLoad) return EHostStartupState::Failed;
		auto Loading = PendingSceneLoad;
		const auto Progress = Loading->GetProgress();
		if (Loading->GetLevel() && Progress.Phase == ESceneLoadPhase::PreparingProducts)
		{
			Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultDocumentAssetLoadComplete);
			if (Loading->GetLevel() == Context.Level)
			{
				Loading->Cancel();
				PendingSceneLoad.reset();
				CompletePendingDocumentOpen(true);
				return EHostStartupState::Ready;
			}
		}
		if (Progress.Phase == ESceneLoadPhase::Ready)
		{
			Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultDocumentActivationBegin);
			if (ActivateLevel(*Loading))
			{
				ActiveSceneLoad = std::move(Loading);
				PendingSceneLoad.reset();
				PendingAction = ELevelDocumentAction::None;
				PendingLevelPath.clear();
				Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultDocumentActivationComplete);
				CompletePendingDocumentOpen(true);
				return EHostStartupState::Ready;
			}
			Loading->Cancel();
		}
		else if (Progress.Phase != ESceneLoadPhase::Failed && Progress.Phase != ESceneLoadPhase::Canceled)
			return EHostStartupState::Pending;
		if (!Progress.Diagnostic.empty()) SetError(Progress.Diagnostic);
		PendingSceneLoad.reset();
		PendingAction = ELevelDocumentAction::None;
		PendingLevelPath.clear();
		CompletePendingDocumentOpen(false);
		return EHostStartupState::Failed;
	}
	auto FLevelDocumentController::CompletePendingDocumentOpen(bool bSucceeded) -> void
	{
		if (!std::exchange(bPendingDocumentOpen, false) || !CompleteDeferredOpen) return;
		CompleteDeferredOpen(bSucceeded);
	}

	auto FLevelDocumentController::CanSaveCurrentLevel() const -> bool
	{
		const auto* Gizmo = SceneViewportPanel.GetTransformGizmo();
		const auto* Transactions = GetLevelTransactions();
		return !PendingSceneLoad && Context.Level && Context.Level->GetPackage() && !(GEditor && GEditor->IsPlaying())
			&& !(Gizmo && Gizmo->IsDragging()) && (!Transactions || (Transactions->CanTransact() && !Transactions->IsActive()));
	}

	auto FLevelDocumentController::SaveCurrentLevel() -> bool
	{
		if (ClearError) ClearError();
		if (!CanSaveCurrentLevel())
		{
			SetError("Saving requires an idle document with no unfinished edit or recovery.");
			return false;
		}
		if (!Context.Level || !Context.Level->GetPackage())
		{
			SetError("The current level is transient and cannot be saved.");
			return false;
		}
		SessionSettings.CaptureViewportState(Context, SceneViewportPanel);
		SessionSettings.Save(&SceneViewportPanel);
		FAssetWriteResult Result = SavePackage(Context.Level->GetPackage());
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
		if (!CanSaveCurrentLevel())
		{
			SetError("Renaming requires an idle document with no unfinished edit or recovery.");
			return false;
		}
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

		FPackagePath OldPath;
		std::string PathError;
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(Package->GetPackagePath(), OldPath); !PathValidation)
		{
			PathError = Durin::ToString(PathValidation.error());
			SetError(PathError);
			return false;
		}
		const std::string OldPathString = OldPath.ToString();
		const size_t Separator = OldPathString.find_last_of('/');
		const std::string NewPathString = OldPathString.substr(0, Separator + 1) + std::string(NewName);
		FPackagePath NewPath;
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(NewPathString, NewPath); !PathValidation)
		{
			PathError = Durin::ToString(PathValidation.error());
			SetError(PathError);
			return false;
		}

		if (OldPath == NewPath)
		{
			if (Context.Level->GetName() == NewName) return true;
			const FName OldObjectName = Context.Level->GetFName();
			const bool bWasDirty = Package->IsDirty();
			Context.Level->Rename(FName(NewName));
			const FAssetWriteResult SaveResult = SavePackage(Package);
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
		const auto MoveResult =
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

	auto FLevelDocumentController::CloseLevel() -> bool
	{
		if (PendingSceneLoad) { PendingSceneLoad->Cancel(); PendingSceneLoad.reset(); CompletePendingDocumentOpen(false); }
		if (DocumentWorld.IsValid() && GEditor && GEditor->IsPlaying()) return false;
		DWorld* World = DocumentWorld.Get();
		DPackage* Package = DocumentPackage.Get();
		if (World && World->GetCurrentLevel())
		{
			// The authoritative world can have been updated by package reload.
			Package = World->GetCurrentLevel()->GetPackage();
			SessionSettings.CaptureViewportState(Context, SceneViewportPanel);
			SessionSettings.Save(&SceneViewportPanel);
			if (!SceneViewportPanel.PrepareForLevelChange(Context)) return false;
			if (!World->SetCurrentLevel(nullptr)) return false;
			FLevelDocumentRevisionState::Activate(GetLevelTransactions(), Package, nullptr);
			Context.Synchronize(World);
			SceneViewportPanel.RestoreCameraState(nullptr, nullptr);
		}
		ActiveSceneLoad.reset();
		DocumentPackage.Reset();
		DocumentWorld.Reset();
		return true;
	}

	auto FLevelDocumentController::ActivateLevel(FSceneLoadOperation& Loading) -> bool
	{
		DLevel* Level = Loading.GetLevel();
		if (!Context.World || !Level)
		{
			SetError("No world is available to activate the level.");
			return false;
		}
		SessionSettings.CaptureViewportState(Context, SceneViewportPanel);
		SessionSettings.Save(&SceneViewportPanel);
		DLevel* Previous = Context.World->GetCurrentLevel();
		DPackage* PreviousPackage = Previous ? Previous->GetPackage() : nullptr;
		if (Previous != Level && !SceneViewportPanel.PrepareForLevelChange(Context)) return false;
		if (!Loading.Commit())
		{
			SetError("The level is already active in another world.");
			return false;
		}
		FLevelDocumentRevisionState::Activate(GetLevelTransactions(), PreviousPackage, Level->GetPackage());
		Context.Synchronize(Context.EditorHost->GetEditorWorld());
		SessionSettings.RestoreViewportState(Level, SceneViewportPanel);
		DocumentPackage = Level->GetPackage();
		DocumentWorld = Context.World;
		return true;
	}

	auto FLevelDocumentController::SetError(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}
} // namespace Durin::Editor::Level
