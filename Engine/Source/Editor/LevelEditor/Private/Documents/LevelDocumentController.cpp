#include "Asset/RegistryOperations.h"
#include "AssetRegistry/Publication.h"
#include "Documents/LevelDocumentController.h"
#include "Documents/LevelDocumentRevisionState.h"
#include "FileHelpers.h"
#include "MonaImGui.h"

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
		FLevelEditorWorkspaceSettings& InWorkspaceSettings,
		FSceneViewportPanel& InSceneViewportPanel,
		FEditorAssetMoveCoordinator& InAssetMoveCoordinator,
		std::function<void()> InLevelChanged,
		std::function<void()> InClearError,
		std::function<void(std::string)> InReportError
	)
		: Context(InContext)
		, WorkspaceSettings(InWorkspaceSettings)
		, SceneViewportPanel(InSceneViewportPanel)
		, AssetMoveCoordinator(InAssetMoveCoordinator)
		, LevelChanged(std::move(InLevelChanged))
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
			}
			else ActivateCurrentLevel();
		});
	}

	FLevelDocumentController::~FLevelDocumentController()
	{
		if (Context.EditorHost) Context.EditorHost->MapChanged.Remove(MapChangedHandle);
	}

	auto FLevelDocumentController::RequestOpenProject(std::string ProjectFile) -> void
	{
		if (!Context.EditorHost || !Context.EditorHost->GetCapabilities().bRunTools || HasPendingTransition())
		{
			SetError("A level transition is already in progress.");
			return;
		}
		PendingPath = std::move(ProjectFile);
		if (IsCurrentLevelDirty())
		{
			PendingLevel = FObjectKey(GetCurrentLevel());
			QueuedPopup = EQueuedPopup::UnsavedLevel;
			return;
		}
		RelaunchEditorForProject(std::exchange(PendingPath, {}));
	}

	auto FLevelDocumentController::RequestCloseLevel() -> bool
	{
		if (!CanEditCurrentLevel() || HasPendingTransition()) return false;
		if (!IsCurrentLevelDirty()) return CloseCurrentLevel();
		PendingLevel = FObjectKey(GetCurrentLevel());
		if (auto* Asset = GetCurrentLevelAsset())
		{
			FEditorFileUtils::FPromptForCheckoutAndSaveParams Prompt{
				.bCheckDirty = true, .bPromptToSave = true, .bIsExplicitSave = true,
				.Title = "Close Level", .Message = "Save the current level before closing?"};
			const auto Response = FEditorFileUtils::PromptForCheckoutAndSave(std::array{Asset->GetPackage()}, Prompt);
			const bool bSameLevel = FObjectKey(GetCurrentLevel()) == std::exchange(PendingLevel, {});
			return bSameLevel && (Response == FEditorFileUtils::PR_Success || Response == FEditorFileUtils::PR_Declined)
				&& CloseCurrentLevel();
		}
		bPendingCloseLevel = true;
		QueuedPopup = EQueuedPopup::UnsavedLevel;
		return false;
	}

	auto FLevelDocumentController::HasPendingTransition() const -> bool
	{
		return bPendingCloseLevel || !PendingPath.empty() || !PendingLevel.IsNull();
	}

	auto FLevelDocumentController::CloseCurrentLevel() -> bool
	{
		auto* Editor = Context.EditorHost;
		const auto Report = ReportError;
		std::string Error;
		if (Editor && Editor->CloseMap(Error)) return true;
		if (Report && !Error.empty()) Report(std::move(Error));
		return false;
	}

	auto FLevelDocumentController::RequestSaveAs() -> void
	{
		if (!CanEditCurrentLevel()) return;
		bOpenSaveAs = true;
		SaveAsPath = {};
		SaveAsError.clear();
		if (PendingLevel.IsNull()) PendingLevel = FObjectKey(GetCurrentLevel());
	}

	auto FLevelDocumentController::DrawDialogs() -> void
	{
		const bool bOpenUnsavedLevel = std::exchange(QueuedPopup, EQueuedPopup::None) == EQueuedPopup::UnsavedLevel;
		(void)UnsavedLevelDialog.Draw(bOpenUnsavedLevel,
			[this](EUnsavedLevelDialogDecision Decision) { return ResolveUnsavedLevelDialog(Decision); });

		if (std::exchange(bOpenSaveAs, false)) ImGui::OpenPopup("Save Level As");
		if (!ImGui::BeginPopupModal("Save Level As", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
		ImGui::InputTextWithHint("Package path", "/Game/Maps/Level", SaveAsPath.data(), SaveAsPath.size());
		if (ImGui::Button("Save"))
		{
			FPackagePath Path;
			if (!FPackagePath::TryCreate(SaveAsPath.data(), Path)) SaveAsError = "Enter a valid package path.";
			else if (FObjectKey(GetCurrentLevel()) != PendingLevel) SaveAsError = "The current level changed.";
			else if (SaveCurrentLevelAs(Path))
			{
				ImGui::CloseCurrentPopup();
				CompletePendingTransition();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			ResolveUnsavedLevelDialog(EUnsavedLevelDialogDecision::Cancel);
			ImGui::CloseCurrentPopup();
		}
		if (!SaveAsError.empty()) ImGui::TextWrapped("%s", SaveAsError.c_str());
		ImGui::EndPopup();
	}

	auto FLevelDocumentController::ResolveUnsavedLevelDialog(EUnsavedLevelDialogDecision Decision) -> bool
	{
		if (Decision == EUnsavedLevelDialogDecision::None) return false;
		if (Decision == EUnsavedLevelDialogDecision::Cancel || FObjectKey(GetCurrentLevel()) != PendingLevel)
		{
			PendingPath.clear();
			PendingLevel = {};
			bPendingCloseLevel = false;
			return true;
		}
		if (Decision == EUnsavedLevelDialogDecision::Save)
		{
			if (!GetCurrentLevelAsset())
			{
				RequestSaveAs();
				return bOpenSaveAs;
			}
			if (!SaveCurrentLevel()) return false;
		}
		return CompletePendingTransition();
	}

	auto FLevelDocumentController::CompletePendingTransition() -> bool
	{
		PendingLevel = {};
		if (std::exchange(bPendingCloseLevel, false)) return CloseCurrentLevel();
		if (!PendingPath.empty()) RelaunchEditorForProject(std::exchange(PendingPath, {}));
		return true;
	}

	auto FLevelDocumentController::GetCurrentLevel() const -> DLevel*
	{
		const auto* World = Context.EditorHost ? Context.EditorHost->GetEditorWorld() : nullptr;
		return World ? World->GetCurrentLevel() : nullptr;
	}

	auto FLevelDocumentController::GetCurrentLevelAsset() const -> DLevel*
	{
		auto* Level = GetCurrentLevel();
		const auto* Package = Level ? Level->GetPackage() : nullptr;
		return Package && Package->IsAssetPackage() && !Level->HasAnyObjectFlags(EObjectFlags::Transient) ? Level : nullptr;
	}

	auto FLevelDocumentController::IsCurrentLevelDirty() const -> bool
	{
		if (const auto* Asset = GetCurrentLevelAsset()) return Asset->GetPackage()->IsDirty();
		const auto* Level = GetCurrentLevel();
		return Level && !Level->GetActors().empty();
	}

	auto FLevelDocumentController::ActivateCurrentLevel() -> void
	{
		auto* World = Context.EditorHost ? Context.EditorHost->GetEditorWorld() : nullptr;
		auto* Level = World ? World->GetCurrentLevel() : nullptr;
		auto* Package = Level ? Level->GetPackage() : nullptr;
		FLevelDocumentRevisionState::Activate(Context.EditorHost ? Context.EditorHost->GetTransactor() : nullptr,
			DocumentPackage.Get(), Package);
		Context.Synchronize(World);
		WorkspaceSettings.RestoreViewportState(Level, SceneViewportPanel);
		DocumentPackage = Package;
		SceneViewportPanel.RequestInitialPresentation();
		if (LevelChanged) LevelChanged();
	}

	auto FLevelDocumentController::CanEditCurrentLevel() const -> bool
	{
		const auto* Gizmo = SceneViewportPanel.GetTransformGizmo();
		const auto* Transactions = Context.EditorHost ? Context.EditorHost->GetTransactor() : nullptr;
		return Context.EditorHost && Context.EditorHost->GetCapabilities().bEditWorld && GetCurrentLevel()
			&& !(Gizmo && Gizmo->IsDragging()) && (!Transactions || (Transactions->CanTransact() && !Transactions->IsActive()));
	}

	auto FLevelDocumentController::CanSaveCurrentLevel() const -> bool
	{
		return CanEditCurrentLevel() && GetCurrentLevelAsset();
	}

	auto FLevelDocumentController::SaveCurrentLevelAs(const FPackagePath& Path) -> bool
	{
		if (!CanEditCurrentLevel()) return false;
		auto* Editor = Context.EditorHost;
		const auto Report = ReportError;
		std::string Error;
		if (Editor->SaveMapAs(Path, Error)) return true;
		if (Report && !Error.empty()) Report(Error);
		SaveAsError = std::move(Error);
		return false;
	}

	auto FLevelDocumentController::SaveCurrentLevel() -> bool
	{
		if (ClearError) ClearError();
		if (!CanSaveCurrentLevel())
		{
			SetError("Saving requires an idle document with no unfinished edit or recovery.");
			return false;
		}
		const auto* Level = GetCurrentLevelAsset();
		const TStrongObjectPtr<DPackage> Package(Level->GetPackage());
		WorkspaceSettings.CaptureViewportState(Context, SceneViewportPanel);
		WorkspaceSettings.Save(&SceneViewportPanel);
		FAssetWriteResult Result;
		const bool Saved = SavePackage(Package.Get(), {}, &Result);
		FLevelDocumentRevisionState::CompleteSave(
			(Context.EditorHost ? Context.EditorHost->GetTransactor() : nullptr), *Package, Saved
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

	auto FLevelDocumentController::ReleaseLevelView() -> bool
	{
		QueuedPopup = EQueuedPopup::None;
		PendingPath.clear();
		WorkspaceSettings.CaptureViewportState(Context, SceneViewportPanel);
		WorkspaceSettings.Save(&SceneViewportPanel);
		if (!SceneViewportPanel.PrepareForLevelChange(Context)) return false;
		Context.Synchronize(nullptr);
		SceneViewportPanel.RestoreCameraState(nullptr, nullptr);
		DocumentPackage.Reset();
		return true;
	}

	auto FLevelDocumentController::SetError(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}
} // namespace Durin::LevelEditor
