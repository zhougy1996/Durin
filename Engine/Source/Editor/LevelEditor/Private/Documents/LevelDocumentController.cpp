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
#include "Engine/MapChange.h"
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
		if (IsOpening())
		{
			SetError("A level transition is already in progress.");
			return;
		}
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

	auto FLevelDocumentController::OpenLevel(std::string Path) -> ELevelDocumentOpenResult
	{
		if (IsOpening())
		{
			SetError("A level transition is already in progress.");
			return ELevelDocumentOpenResult::Rejected;
		}
		if (!Context.EditorHost || Context.EditorHost->IsPlaying()) return ELevelDocumentOpenResult::Rejected;
		PendingProjectFile.clear();
		FObjectPath AssetPath;
		if (!FObjectPath::TryCreate(Path, AssetPath))
		{
			SetError("Invalid level asset path.");
			return ELevelDocumentOpenResult::Rejected;
		}
		const FObjectPathResolveResult Resolution =
			ResolveAssetObjectPathForOperation(
				AssetPath, {.ExpectedClass = DLevel::StaticClass()});
		if (!Resolution)
		{
			SetError("The level asset could not be resolved.");
			return ELevelDocumentOpenResult::Rejected;
		}
		PendingLevelPath = std::move(Path);
		PendingAction = ELevelDocumentAction::OpenLevel;
		bPendingDocumentOpen = true;
		if (Context.Level && Context.Level->GetPackage() && Context.Level->GetPackage()->IsDirty())
		{
			QueuedPopup = EQueuedPopup::UnsavedLevel;
			return ELevelDocumentOpenResult::AwaitingConfirmation;
		}
		const ELevelDocumentOpenResult Result = LoadLevel(PendingLevelPath);
		if (Result != ELevelDocumentOpenResult::Loading)
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
				const ELevelDocumentOpenResult Result = LoadLevel(PendingLevelPath);
				if (Result != ELevelDocumentOpenResult::Loading)
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

	auto FLevelDocumentController::IsOpening() const -> bool
	{
		return bPendingDocumentOpen || (Context.EditorHost && Context.EditorHost->IsPreparingMapChange());
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
		std::optional<FSlowTaskSnapshot> Feedback;
		if (Context.EditorHost && Context.EditorHost->IsPreparingMapChange() && bShowLoadingProgress)
		{
			const auto Progress = *Context.EditorHost->GetMapChangeProgress();
			if (Progress.Phase != EMapChangePhase::Failed && Progress.Phase != EMapChangePhase::Canceled
				&& Progress.Phase != EMapChangePhase::Committed)
			{
				const std::string_view Path = PendingLevelPath;
				const auto Separator = Path.find_last_of('/');
				const auto Name = Path.substr(Separator == std::string_view::npos ? 0 : Separator + 1);
				Feedback = FSlowTaskSnapshot{.Id = Progress.Id,
					.Title = std::format("Opening {}...", Name.substr(0, Name.find('.'))),
					.Message = Progress.Phase == EMapChangePhase::Ready ? "Activating level" : std::string(GetMapChangePhaseLabel(Progress.Phase)),
					.Details = PendingLevelPath, .ElapsedSeconds = Progress.ElapsedSeconds, .bCanCancel = true};
			}
		}
		if (LoadingDialog.Draw(Feedback) && Context.EditorHost) Context.EditorHost->CancelPendingMapChange();
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
		if (bCompletesDeferredOpen && Result != ELevelDocumentOpenResult::Loading)
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
			if (LoadLevel(DefaultLevel.GetPath().ToString(), false) == ELevelDocumentOpenResult::Rejected)
				return DefaultOpenState = EHostStartupState::Failed;
		}
		return DefaultOpenState;
	}

	auto FLevelDocumentController::LoadLevel(std::string_view PathString, bool bShowProgress) -> ELevelDocumentOpenResult
	{
		if (ClearError) ClearError();
		if (!Context.EditorHost || Context.EditorHost->IsPlaying()) return ELevelDocumentOpenResult::Rejected;
		FObjectPath Path;
		if (const auto Validation = FObjectPath::TryCreateWithDiagnostic(PathString, Path); !Validation)
		{
			SetError(Durin::ToString(Validation.error()));
			return ELevelDocumentOpenResult::Rejected;
		}
		if (Context.EditorHost->IsPreparingMapChange()) return ELevelDocumentOpenResult::Rejected;
		if (Context.Level && Context.Level->GetObjectPath() == Path.ToString())
		{
			if (!bShowProgress) DefaultOpenState = EHostStartupState::Ready;
			return ELevelDocumentOpenResult::Opened;
		}
		bShowLoadingProgress = bShowProgress;
		if (!CallbackLifetime) CallbackLifetime = std::make_shared<uint8>(0);
		const std::weak_ptr<void> Lifetime = CallbackLifetime;
		if (!Context.EditorHost->OpenLevel({.Path = Path,
			.BuildInitialView = SessionSettings.CreateInitialViewBuilder(SceneViewportPanel),
			.BeforeCommit = [this, Lifetime](DLevel& Level) { return !Lifetime.expired() && PrepareLevelActivation(Level); },
			.OnComplete = [this, Lifetime](const FMapChangeProgress& Progress) { if (!Lifetime.expired()) CompleteMapChange(Progress); }}))
		{
			SetError("Unable to start loading the level.");
			return ELevelDocumentOpenResult::Rejected;
		}
		Context.bReadOnly = true;
		return ELevelDocumentOpenResult::Loading;
	}

	auto FLevelDocumentController::CompleteMapChange(const FMapChangeProgress& Progress) -> void
	{
		const bool bSucceeded = Progress.Phase == EMapChangePhase::Committed;
		if (bSucceeded)
		{
			auto* World = Context.EditorHost->GetEditorWorld();
			auto* Level = World->GetCurrentLevel();
			FLevelDocumentRevisionState::Activate(GetLevelTransactions(), DocumentPackage.Get(), Level->GetPackage());
			Context.Synchronize(World);
			SessionSettings.RestoreViewportState(Level, SceneViewportPanel);
			DocumentPackage = Level->GetPackage();
			DocumentWorld = World;
			Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultDocumentActivationComplete);
		}
		else if (Progress.Phase == EMapChangePhase::Failed)
			SetError(Progress.Diagnostic.empty() ? "Failed to load the level." : Progress.Diagnostic);
		if (bDefaultStarted && DefaultOpenState == EHostStartupState::Pending)
			DefaultOpenState = bSucceeded ? EHostStartupState::Ready : EHostStartupState::Failed;
		PendingAction = ELevelDocumentAction::None;
		PendingLevelPath.clear();
		CompletePendingDocumentOpen(bSucceeded);
	}
	auto FLevelDocumentController::CompletePendingDocumentOpen(bool bSucceeded) -> void
	{
		Context.bReadOnly = Context.EditorHost && (Context.EditorHost->IsPlaying()
			|| Context.EditorHost->GetStartupState() != EHostStartupState::Ready);
		if (!std::exchange(bPendingDocumentOpen, false) || !CompleteDeferredOpen) return;
		CompleteDeferredOpen(bSucceeded);
	}

	auto FLevelDocumentController::CanSaveCurrentLevel() const -> bool
	{
		const auto* Gizmo = SceneViewportPanel.GetTransformGizmo();
		const auto* Transactions = GetLevelTransactions();
		return !(Context.EditorHost && Context.EditorHost->IsPreparingMapChange()) && Context.Level && Context.Level->GetPackage() && !(GEditor && GEditor->IsPlaying())
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
		CallbackLifetime.reset();
		if (Context.EditorHost) Context.EditorHost->CancelPendingMapChange();
		PendingAction = ELevelDocumentAction::None;
		QueuedPopup = EQueuedPopup::None;
		PendingLevelPath.clear();
		PendingProjectFile.clear();
		CompletePendingDocumentOpen(false);
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
		DocumentPackage.Reset();
		DocumentWorld.Reset();
		return true;
	}

	auto FLevelDocumentController::PrepareLevelActivation(DLevel& Level) -> bool
	{
		if (!Context.World)
		{
			SetError("No world is available to activate the level.");
			return false;
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultDocumentAssetLoadComplete);
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::DefaultDocumentActivationBegin);
		SessionSettings.CaptureViewportState(Context, SceneViewportPanel);
		SessionSettings.Save(&SceneViewportPanel);
		if (const auto* Previous = Context.World->GetCurrentLevel()) DocumentPackage = Previous->GetPackage();
		return Context.World->GetCurrentLevel() == &Level || SceneViewportPanel.PrepareForLevelChange(Context);
	}

	auto FLevelDocumentController::SetError(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}
} // namespace Durin::Editor::Level
