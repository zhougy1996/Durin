#include "Asset/RegistryOperations.h"
#include "AssetRegistry/Publication.h"
#include "Documents/LevelDocumentController.h"
#include "Documents/EditorDocumentManager.h"
#include "Documents/LevelDocumentRevisionState.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/Asset.h"
#include "Editor/EditorEngine.h"
#include "Transactions/Transaction.h"
#include "Settings/LevelEditorWorkspaceSettings.h"
#include "Assets/EditorAssetMoveCoordinator.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Workspace/LevelEditorContext.h"
#include "Project/Project.h"
#include "Panels/SceneViewportPanel.h"
#include "Viewport/TransformGizmo.h"

namespace Durin::LevelEditor
{
	// Owns level document transitions and the unsaved-change workflow.
	FLevelDocumentController::FLevelDocumentController(
		FLevelEditorContext& InContext,
		FEditorDocumentManager& InDocuments,
		FLevelEditorWorkspaceSettings& InWorkspaceSettings,
		FSceneViewportPanel& InSceneViewportPanel,
		FEditorAssetMoveCoordinator& InAssetMoveCoordinator,
		std::function<void()> InClearError,
		std::function<void(std::string)> InReportError
	)
		: Context(InContext)
		, Documents(InDocuments)
		, WorkspaceSettings(InWorkspaceSettings)
		, SceneViewportPanel(InSceneViewportPanel)
		, AssetMoveCoordinator(InAssetMoveCoordinator)
		, ClearError(std::move(InClearError))
		, ReportError(std::move(InReportError))
	{
		if (Context.EditorHost) MapChangedHandle = Context.EditorHost->MapChanged.AddLambda([this](DWorld*, EMapChangeType Type) {
			if (Type == EMapChangeType::TearDownWorld)
			{
				WorkspaceSettings.CaptureViewportState(Context, SceneViewportPanel);
				WorkspaceSettings.Save(&SceneViewportPanel);
				if (!SceneViewportPanel.PrepareForLevelChange(Context))
					throw std::runtime_error("Unable to release viewport edits before world teardown.");
				Context.Synchronize(nullptr);
				SceneViewportPanel.RestoreCameraState(nullptr, nullptr);
				DocumentPackage.Reset();
				for (const auto& Document : Documents.GetDocuments()) Documents.AssociateDocumentAsset(Document.Id, nullptr);
			}
			else AssociateCurrentLevel();
		});
	}

	FLevelDocumentController::~FLevelDocumentController()
	{
		if (Context.EditorHost) Context.EditorHost->MapChanged.Remove(MapChangedHandle);
	}

	auto FLevelDocumentController::RequestOpenProject(std::string ProjectFile) -> void
	{
		if (Context.EditorHost && !Context.EditorHost->GetCapabilities().bRunTools)
		{
			SetError("A level transition is already in progress.");
			return;
		}
		PendingPath = std::move(ProjectFile);
		if (Context.Level && Context.Level->GetPackage() && Context.Level->GetPackage()->IsDirty())
		{
			QueuedPopup = EQueuedPopup::UnsavedLevel;
			return;
		}
		RelaunchEditorForProject(std::exchange(PendingPath, {}));
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

			PendingPath.clear();
			return true;
		}
		if (Decision != EUnsavedLevelDialogDecision::Save
			&& Decision != EUnsavedLevelDialogDecision::Discard)
			return false;
		RelaunchEditorForProject(std::exchange(PendingPath, {}));
		return true;
	}

	auto FLevelDocumentController::AssociateCurrentLevel() -> bool
	{
		auto* World = Context.EditorHost ? Context.EditorHost->GetEditorWorld() : nullptr;
		if (!World) return false;
		auto* Level = World->GetCurrentLevel();
		if (Level)
		{
			FLevelDocumentRevisionState::Activate((Context.EditorHost ? Context.EditorHost->GetTransactor() : nullptr), DocumentPackage.Get(), Level->GetPackage());
			Context.Synchronize(World);
			WorkspaceSettings.RestoreViewportState(Level, SceneViewportPanel);
			DocumentPackage = Level->GetPackage();
		}
		else Context.Synchronize(World);
		if (Level)
		{
			if (Level->HasAnyObjectFlags(EObjectFlags::Transient))
			{
				if (auto* Document = Documents.GetActiveDocument())
				{ Document->ResourceId.clear(); Document->Label = "Untitled"; Documents.AssociateDocumentAsset(Document->Id, nullptr); }
			}
			else for (const auto& Document : Documents.GetDocuments()) Documents.AssociateDocumentAsset(Document.Id, Level);
		}
		SceneViewportPanel.RequestInitialPresentation();
		return Level != nullptr;
	}

	auto FLevelDocumentController::CanSaveCurrentLevel() const -> bool
	{
		const auto* Gizmo = SceneViewportPanel.GetTransformGizmo();
		const auto* Transactions = (Context.EditorHost ? Context.EditorHost->GetTransactor() : nullptr);
		return (!Context.EditorHost || Context.EditorHost->GetCapabilities().bEditWorld) && Context.Level && Context.Level->GetPackage()
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
		WorkspaceSettings.CaptureViewportState(Context, SceneViewportPanel);
		WorkspaceSettings.Save(&SceneViewportPanel);
		FAssetWriteResult Result;
		const bool Saved = SavePackage(Context.Level->GetPackage(), {}, &Result);
		FLevelDocumentRevisionState::CompleteSave(
			(Context.EditorHost ? Context.EditorHost->GetTransactor() : nullptr), *Context.Level->GetPackage(), Saved
		);
		if (!Saved)
		{
			SetError((Result.Error ? Result.Error->ToString() : std::string{}));
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
			PathError = ToString(PathValidation.error());
			SetError(PathError);
			return false;
		}
		const std::string OldPathString = OldPath.ToString();
		const size_t Separator = OldPathString.find_last_of('/');
		const std::string NewPathString = OldPathString.substr(0, Separator + 1) + std::string(NewName);
		FPackagePath NewPath;
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(NewPathString, NewPath); !PathValidation)
		{
			PathError = ToString(PathValidation.error());
			SetError(PathError);
			return false;
		}

		const FEditorAssetMove Move{TWeakObjectPtr<DObject>(Context.Level), OldPathString.substr(0, Separator), NewName};
		std::string MoveError;
		const bool MoveResult = AssetMoveCoordinator.MoveAssets(std::span{&Move, 1}, &MoveError);
		auto& RenamedPackage = *Context.Level->GetPackage();
		FLevelDocumentRevisionState::CompleteSave(
			(Context.EditorHost ? Context.EditorHost->GetTransactor() : nullptr), RenamedPackage,
			MoveResult && !RenamedPackage.IsDirty() && !RenamedPackage.IsNewlyCreated()
				&& !IsAssetRegistryProjectionFenced(RenamedPackage.GetPackagePathIdentity())
		);
		if (!MoveError.empty()) SetError(MoveError);
		return MoveResult;
	}

	auto FLevelDocumentController::CloseDocument() -> bool
	{
		QueuedPopup = EQueuedPopup::None;
		PendingPath.clear();
		WorkspaceSettings.CaptureViewportState(Context, SceneViewportPanel);
		WorkspaceSettings.Save(&SceneViewportPanel);
		if (!SceneViewportPanel.PrepareForLevelChange(Context)) return false;
		Context.Synchronize(nullptr);
		SceneViewportPanel.RestoreCameraState(nullptr, nullptr);
		for (const auto& Document : Documents.GetDocuments()) Documents.AssociateDocumentAsset(Document.Id, nullptr);
		DocumentPackage.Reset();
		return true;
	}

	auto FLevelDocumentController::SetError(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}
} // namespace Durin::LevelEditor
