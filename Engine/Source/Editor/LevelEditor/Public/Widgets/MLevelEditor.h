#pragma once

#include "LevelEditorAPI.h"
#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceRootWindow.h"

namespace Durin
{
	enum class EEditorPlayStartLocation : uint8;
	enum class EEditorPlayDestination : uint8;
	class ILevelEditorPanel;
	class FLevelEditorSessionSettings;
	class FEditorAssetMoveCoordinator;
	class FEditorWorkspaceManager;
	class FLevelDocumentController;
	class FSceneViewportPanel;
	class FSceneImportDialog;
	class FStaticMeshImportDialog;
	class FTextureImportDialog;
	class FTextureCubeImportDialog;
	class FContentBrowserPanel;
	class FDetailsPanel;
	class FEditorNotificationOverlay;
	struct FLevelEditorContext;

	// Hosts level documents, panels, play controls, and project settings.
	class MLevelEditor final : public IEditorWorkspace
	{
	public:
		MLevelEditor(FLevelEditorSessionSettings& InSessionSettings, FEditorWorkspaceManager& InWorkspaceManager);
		LEVELEDITOR_API ~MLevelEditor() override;
		LEVELEDITOR_API auto Construct() -> void;
		LEVELEDITOR_API auto GetWorkspaceType() const -> const FEditorWorkspaceTypeId& override;
		LEVELEDITOR_API auto OpenDocument(const FEditorDocumentTab& Document) -> EEditorDocumentOpenResult override;
		LEVELEDITOR_API auto ActivateDocument(const FEditorDocumentTab& Document) -> void override;
		LEVELEDITOR_API auto RequestDeactivate() -> bool override;
		LEVELEDITOR_API auto RequestCloseDocument(const FEditorDocumentTab& Document) -> EEditorDocumentCloseResult override;
		LEVELEDITOR_API auto SaveDocument(const FEditorDocumentTab& Document) -> bool override;
		LEVELEDITOR_API auto DiscardDocument(const FEditorDocumentTab& Document) -> bool override;
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
		LEVELEDITOR_API auto DrawWindowMenu() -> void override;
		LEVELEDITOR_API auto DrawWorkspace(bool bActive) -> bool override;
		LEVELEDITOR_API auto ResetLayout() -> void override;

	private:
		auto DrawProjectSettings() -> void;
		auto LoadProjectSettings() -> bool;
		auto SaveProjectSettings() -> bool;
		auto SetError(std::string Message) -> void;
		auto StartPlay(EEditorPlayStartLocation StartLocation, EEditorPlayDestination Destination) -> void;
		auto ApplyPlayChanges(bool bSelectedOnly) -> void;
		auto BuildDefaultLayout(uint32 DockSpaceId, float DockSpaceWidth, float DockSpaceHeight) -> void;

		std::unique_ptr<FLevelEditorContext> Context;
		// Module-owned services outlive this registered workspace.
		FLevelEditorSessionSettings& SessionSettings;
		FEditorWorkspaceManager& WorkspaceManager;
		std::unique_ptr<FLevelDocumentController> DocumentController;
		std::unique_ptr<FEditorAssetMoveCoordinator> AssetMoveCoordinator;
		std::unique_ptr<FSceneImportDialog> SceneImportDialog;
		std::unique_ptr<FStaticMeshImportDialog> StaticMeshImportDialog;
		std::unique_ptr<FTextureImportDialog> TextureImportDialog;
		std::unique_ptr<FTextureCubeImportDialog> TextureCubeImportDialog;
		FEditorNotificationOverlay* NotificationOverlay = nullptr;
		std::vector<std::unique_ptr<ILevelEditorPanel>> Panels;
		// Panel pointers are non-owning aliases into the Panels collection.
		FSceneViewportPanel* SceneViewportPanel = nullptr;
		FContentBrowserPanel* ContentBrowserPanel = nullptr;
		FDetailsPanel* DetailsPanel = nullptr;
		FEditorWorkspaceRootWindow RootWindow;
		bool bResetLayoutRequested = false;
		bool bSelectDefaultBottomPanelRequested = true;
		bool bWasActive = false;
		bool bProjectSettingsOpen = false;
		std::string DefaultLevel;
		std::string PendingDefaultLevel;
		std::string EditorError;
		FEditorDocumentId DeferredOpenDocumentId;
	};
} // namespace Durin
