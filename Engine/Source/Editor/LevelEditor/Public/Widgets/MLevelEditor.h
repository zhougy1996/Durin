#pragma once

#include "LevelEditorAPI.h"
#include "DObject/SoftObjectPtr.h"
#include "Editor/Workspace.h"
#include "Editor/WorkspaceRootWindow.h"

namespace Durin
{
	enum class EEditorPlayStartLocation : uint8;
	enum class EEditorPlayDestination : uint8;
	class ILevelEditorPanel;
	class FLevelEditorSessionSettings;
	class DLevel;
	class FAssetPath;
	class FEditorAssetMoveCoordinator;
	namespace Editor
	{
		class FWorkspaceManager;
	}
	class FLevelDocumentController;
	class FSceneViewportPanel;
	class FSceneImportDialog;
	class FStaticMeshImportDialog;
	class FTextureImportDialog;
	class FTextureCubeImportDialog;
	class FContentBrowserPanel;
	struct FMountedContentReconciliationState;
	class FDetailsPanel;
	class FEditorNotificationOverlay;
	struct FLevelEditorContext;

	// Hosts level documents, panels, play controls, and project settings.
	class MLevelEditor final : public Editor::IWorkspace
	{
	public:
		MLevelEditor(FLevelEditorSessionSettings& InSessionSettings, Editor::FWorkspaceManager& InWorkspaceManager);
		LEVELEDITOR_API ~MLevelEditor() override;
		LEVELEDITOR_API auto Construct() -> void;
		LEVELEDITOR_API auto OpenDefaultDocument() -> bool;
		LEVELEDITOR_API auto GetWorkspaceType() const -> const Editor::FWorkspaceTypeId& override;
		LEVELEDITOR_API auto OpenDocument(const Editor::FDocumentTab& Document) -> Editor::EDocumentOpenResult override;
		LEVELEDITOR_API auto ActivateDocument(const Editor::FDocumentTab& Document) -> void override;
		LEVELEDITOR_API auto RequestDeactivate() -> bool override;
		LEVELEDITOR_API auto RequestCloseDocument(const Editor::FDocumentTab& Document) -> Editor::EDocumentCloseResult override;
		LEVELEDITOR_API auto SaveDocument(const Editor::FDocumentTab& Document) -> bool override;
		LEVELEDITOR_API auto DiscardDocument(const Editor::FDocumentTab& Document) -> bool override;
		LEVELEDITOR_API auto IsDocumentDirty(const Editor::FDocumentTab& Document) const -> bool override;
		LEVELEDITOR_API auto CanSaveActiveDocument() const -> bool override;
		LEVELEDITOR_API auto SaveActiveDocument() -> bool override;
		LEVELEDITOR_API auto CanUndo() const -> bool override;
		LEVELEDITOR_API auto CanRedo() const -> bool override;
		LEVELEDITOR_API auto GetUndoDescription() const -> std::string_view override;
		LEVELEDITOR_API auto GetRedoDescription() const -> std::string_view override;
		LEVELEDITOR_API auto Undo() -> bool override;
		LEVELEDITOR_API auto Redo() -> bool override;
		LEVELEDITOR_API auto DrawFileMenu() -> void override;
		LEVELEDITOR_API auto DrawEditMenu() -> void override;
		LEVELEDITOR_API auto DrawWindowMenu() -> void override;
		LEVELEDITOR_API auto DrawWorkspace(bool bActive) -> bool override;
		LEVELEDITOR_API auto ResetLayout() -> void override;
		LEVELEDITOR_API auto RevealAssetInContentBrowser(const FAssetPath& AssetPath) -> bool;

	private:
		friend class FLevelEditorModule;

		auto InitializeContext() -> void;
		auto InitializeSession() -> void;
		auto CreatePanels() -> void;
		auto CreateDocumentServices() -> void;
		auto CreateImportDialogs() -> void;
		auto CreateContentBrowser() -> void;
		auto CreateNotificationOverlay() -> void;
		auto FinalizeSessionConstruction() -> void;
		auto DrawProjectSettings() -> void;
		auto LoadProjectSettings() -> bool;
		auto SaveProjectSettings() -> bool;
		auto ApplyFixedUpDefaultLevelPath(const FAssetPath& Path) -> void;
		auto SetError(std::string Message) -> void;
		auto StartPlay(EEditorPlayStartLocation StartLocation, EEditorPlayDestination Destination) -> void;
		auto ApplyPlayChanges(bool bSelectedOnly) -> void;
		auto BuildDefaultLayout(uint32 DockSpaceId, float DockSpaceWidth, float DockSpaceHeight) -> void;

		std::unique_ptr<FLevelEditorContext> Context;
		// Module-owned services outlive this registered workspace.
		FLevelEditorSessionSettings& SessionSettings;
		Editor::FWorkspaceManager& WorkspaceManager;
		std::unique_ptr<FLevelDocumentController> DocumentController;
		std::unique_ptr<FEditorAssetMoveCoordinator> AssetMoveCoordinator;
		std::unique_ptr<FSceneImportDialog> SceneImportDialog;
		std::unique_ptr<FStaticMeshImportDialog> StaticMeshImportDialog;
		std::unique_ptr<FTextureImportDialog> TextureImportDialog;
		std::unique_ptr<FTextureCubeImportDialog> TextureCubeImportDialog;
		std::shared_ptr<FMountedContentReconciliationState>
			MountedContentReconciliationState;
		FEditorNotificationOverlay* NotificationOverlay = nullptr;
		std::vector<std::unique_ptr<ILevelEditorPanel>> Panels;
		// Panel pointers are non-owning aliases into the Panels collection.
		FSceneViewportPanel* SceneViewportPanel = nullptr;
		FContentBrowserPanel* ContentBrowserPanel = nullptr;
		FDetailsPanel* DetailsPanel = nullptr;
		Editor::FWorkspaceRootWindow RootWindow;
		bool bResetLayoutRequested = false;
		bool bSelectDefaultBottomPanelRequested = true;
		bool bWasActive = false;
		bool bProjectSettingsOpen = false;
		TSoftObjectPtr<DLevel> DefaultLevel;
		TSoftObjectPtr<DLevel> PendingDefaultLevel;
		std::string EditorError;
		Editor::FDocumentId DeferredOpenDocumentId;
	};
} // namespace Durin
