#pragma once

#include "LevelEditorAPI.h"
#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceRootWindow.h"

namespace Durin
{
	enum class EEditorPlayStartLocation : uint8;
	enum class EEditorPlayDestination : uint8;
	class ILevelEditorPanel;
	class FEditorSessionSettings;
	class FEditorAssetMoveCoordinator;
	class FEditorWorkspaceManager;
	class FLevelDocumentController;
	class FSceneViewportPanel;
	class FStaticMeshImportDialog;
	class FTextureImportDialog;
	class FContentBrowserPanel;
	class FEditorNotificationOverlay;
	struct FLevelEditorContext;

	class MLevelEditor final : public IEditorWorkspace
	{
	public:
		MLevelEditor(FEditorSessionSettings& InSessionSettings, FEditorWorkspaceManager& InWorkspaceManager);
		LEVELEDITOR_API ~MLevelEditor() override;
		LEVELEDITOR_API auto Construct() -> void;
		LEVELEDITOR_API auto GetWorkspaceType() const -> const FEditorWorkspaceTypeId& override;
		LEVELEDITOR_API auto OpenDocument(const FEditorDocumentTab& Document) -> bool override;
		LEVELEDITOR_API auto ActivateDocument(const FEditorDocumentTab& Document) -> void override;
		LEVELEDITOR_API auto RequestCloseDocument(const FEditorDocumentTab& Document) -> bool override;
		LEVELEDITOR_API auto IsDocumentDirty(const FEditorDocumentTab& Document) const -> bool override;
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
		LEVELEDITOR_API auto DrawApplicationMenus() -> void override;
		LEVELEDITOR_API auto DrawWindowMenu() -> void override;
		LEVELEDITOR_API auto DrawWorkspace(bool bActive) -> bool override;
		LEVELEDITOR_API auto ResetLayout() -> void override;

	private:
		auto DrawAboutDialog() -> void;
		auto DrawProjectSettings() -> void;
		auto ApplyDisplaySettings(int32 Width, int32 Height, float Scale) -> void;
		auto LoadProjectSettings() -> bool;
		auto SaveProjectSettings() -> bool;
		auto SetError(std::string Message) -> void;
		auto StartPlay(EEditorPlayStartLocation StartLocation, EEditorPlayDestination Destination) -> void;
		auto ApplyPlayChanges(bool bSelectedOnly) -> void;
		auto BuildDefaultLayout(uint32 DockSpaceId, float DockSpaceWidth, float DockSpaceHeight) -> void;

		std::unique_ptr<FLevelEditorContext> Context;
		FEditorSessionSettings& SessionSettings;
		FEditorWorkspaceManager& WorkspaceManager;
		std::unique_ptr<FLevelDocumentController> DocumentController;
		std::unique_ptr<FEditorAssetMoveCoordinator> AssetMoveCoordinator;
		std::unique_ptr<FStaticMeshImportDialog> StaticMeshImportDialog;
		std::unique_ptr<FTextureImportDialog> TextureImportDialog;
		FEditorNotificationOverlay* NotificationOverlay = nullptr;
		std::vector<std::unique_ptr<ILevelEditorPanel>> Panels;
		FSceneViewportPanel* SceneViewportPanel = nullptr;
		FContentBrowserPanel* ContentBrowserPanel = nullptr;
		FEditorWorkspaceRootWindow RootWindow;
		bool bResetLayoutRequested = false;
		bool bSelectDefaultBottomPanelRequested = true;
		bool bProjectSettingsOpen = false;
		bool bAboutDialogOpen = false;
		std::string DefaultLevel;
		std::string EditorError;
	};
} // namespace Durin
