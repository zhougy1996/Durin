#pragma once

#include "CoreMinimal.h"

#include "Documents/DocumentDialogPresenters.h"
#include "Asset/Load.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	class FEditorDocumentManager;
	class DLevel;
	class DWorld;
}

namespace Durin::LevelEditor
{
	class FEditorAssetMoveCoordinator;
	class FLevelEditorWorkspaceSettings;
	struct FLevelEditorContext;
	class FSceneViewportPanel;

	// Coordinates new/open/save/close flows and their confirmation popups.
	class FLevelDocumentController
	{
	public:
		FLevelDocumentController(
			FLevelEditorContext& InContext,
			FEditorDocumentManager& InDocuments,
			FLevelEditorWorkspaceSettings& InWorkspaceSettings,
			FSceneViewportPanel& InSceneViewportPanel,
			FEditorAssetMoveCoordinator& InAssetMoveCoordinator,
			std::function<void()> InClearError,
			std::function<void(std::string)> InReportError
		);

		~FLevelDocumentController();
		auto RequestOpenProject(std::string ProjectFile) -> void;
		auto DrawDialogs() -> void;
		auto CanSaveCurrentLevel() const -> bool;
		auto SaveCurrentLevel() -> bool;
		auto RenameCurrentLevel(std::string_view NewName) -> bool;
		auto CloseDocument() -> bool;

	private:
		friend class FLevelEditorToolkit;
		// Selects the modal confirmation that must be resolved before continuing.
		enum class EQueuedPopup
		{
			None,
			UnsavedLevel
		};

		auto ResolveUnsavedLevelDialog(EUnsavedLevelDialogDecision Decision) -> bool;
		auto AssociateCurrentLevel() -> bool;
		auto SetError(std::string Message) const -> void;

		FDelegateHandle MapChangedHandle;
		FLevelEditorContext& Context;
		FEditorDocumentManager& Documents;
		FLevelEditorWorkspaceSettings& WorkspaceSettings;
		FSceneViewportPanel& SceneViewportPanel;
		FEditorAssetMoveCoordinator& AssetMoveCoordinator;
		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		EQueuedPopup QueuedPopup = EQueuedPopup::None;
		std::string PendingPath;
		FUnsavedLevelDialogPresenter UnsavedLevelDialog;
		TWeakObjectPtr<DPackage> DocumentPackage;
	};
} // namespace Durin::LevelEditor
