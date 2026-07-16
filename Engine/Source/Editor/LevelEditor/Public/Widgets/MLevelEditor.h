#pragma once

#include "LevelEditorAPI.h"
#include "Editor/EditorWorkspace.h"

namespace Durin
{
	class ILevelEditorPanel;
	class FEditorSessionSettings;
	class FEditorWorkspaceManager;
	class FLevelDocumentController;
	class FSceneViewportPanel;
	class FStaticMeshImportDialog;
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
		LEVELEDITOR_API auto DrawMainMenu() -> void override;
		LEVELEDITOR_API auto DrawWorkspace(bool bActive) -> bool override;
		LEVELEDITOR_API auto ResetLayout() -> void override;

	private:
		auto DrawProjectSettings() -> void;
		auto ApplyDisplaySettings(int32 Width, int32 Height, float Scale) -> void;
		auto LoadProjectSettings() -> bool;
		auto SaveProjectSettings() -> bool;
		auto SetError(std::string Message) -> void;
		auto BuildDefaultLayout(uint32 DockSpaceId, float DockSpaceWidth, float DockSpaceHeight) -> void;

		std::unique_ptr<FLevelEditorContext> Context;
		FEditorSessionSettings& SessionSettings;
		FEditorWorkspaceManager& WorkspaceManager;
		std::unique_ptr<FLevelDocumentController> DocumentController;
		std::unique_ptr<FStaticMeshImportDialog> StaticMeshImportDialog;
		std::unique_ptr<FEditorNotificationOverlay> NotificationOverlay;
		std::vector<std::unique_ptr<ILevelEditorPanel>> Panels;
		FSceneViewportPanel* SceneViewportPanel = nullptr;
		FContentBrowserPanel* ContentBrowserPanel = nullptr;
		bool bResetLayoutRequested = false;
		bool bFocusRequested = false;
		bool bProjectSettingsOpen = false;
		bool bActivityHistoryOpen = false;
		std::string DefaultLevel;
		std::string EditorError;
	};
} // namespace Durin
