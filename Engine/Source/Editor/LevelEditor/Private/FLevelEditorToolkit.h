#pragma once

#include "CoreMinimal.h"
#include "Engine/HostStartup.h"

#include "LevelEditorAPI.h"
#include "LevelEditorContentBrowserCallbacks.h"
#include "DObject/SoftObjectPtr.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Framework/TabManager.h"
#include "Settings/EditorSettings.h"

namespace Durin
{
	class DLevel;
	class FPackagePath;
	class FLevelEditorModule;
}

namespace Durin
{
	enum class EPlayStartLocation : uint8;
	enum class EPlayDestination : uint8;

}

namespace Durin::LevelEditor
{
	class ILevelEditorPanel;
	class FLevelEditorWorkspaceSettings;
	class FEditorAssetMoveCoordinator;
	class FLevelDocumentController;
	class FSceneViewportPanel;
	class FRenderingDiagnosticsPanel;
	class FSceneImportDialog;
	class FDetailsPanel;
	struct FLevelEditorContext;

	// Hosts level documents, panels, play controls, and project settings.
	class FLevelEditorToolkit final : public FBaseAssetToolkit
	{
	public:
		FLevelEditorToolkit(std::shared_ptr<FLevelEditorWorkspaceSettings> InWorkspaceSettings,
			FContentBrowserCallbacks InContentBrowserCallbacks,
			std::function<void()> InOpenProjectBrowser, DEditorEngine& EditorHost);
		LEVELEDITOR_API auto OnClose() -> void override;
		LEVELEDITOR_API ~FLevelEditorToolkit() override;
		LEVELEDITOR_API auto Construct() -> void;
		LEVELEDITOR_API auto GetToolkitId() const -> const FAssetEditorToolkitId& override;
		LEVELEDITOR_API auto GetEditorDisplayName() const -> std::string override;
		auto GetLastOperationError() const -> std::string override { return EditorError; }
		auto GetOwnedEditingObjects() const -> std::optional<std::vector<DObject*>> override;
		LEVELEDITOR_API auto RequestDeactivate() -> bool override;
		auto OnRequestClose(EAssetEditorCloseReason Reason) -> bool override;
		auto PrepareForExit() -> bool override;
		auto SaveForExit() -> bool override;
		auto GetUnsavedChanges() const -> std::vector<FAssetEditorUnsavedChange> override;
		LEVELEDITOR_API auto RequestCloseLevel() -> bool;
		LEVELEDITOR_API auto SaveLevelAs(const FPackagePath& Path) -> bool;
		LEVELEDITOR_API auto OnCanSaveActiveDocument() const -> bool override;
		LEVELEDITOR_API auto OnSaveActiveDocument() -> bool override;
		LEVELEDITOR_API auto OnCanUndo() const -> bool override;
		LEVELEDITOR_API auto OnCanRedo() const -> bool override;
		LEVELEDITOR_API auto GetUndoDescription() const -> std::string override;
		LEVELEDITOR_API auto GetRedoDescription() const -> std::string override;
		LEVELEDITOR_API auto OnUndo() -> bool override;
		LEVELEDITOR_API auto OnRedo() -> bool override;
		LEVELEDITOR_API auto DrawFileMenu() -> void override;
		LEVELEDITOR_API auto DrawEditMenu() -> void override;
		LEVELEDITOR_API auto DrawWindowMenu() -> void override;
		LEVELEDITOR_API auto OnDrawToolkit(bool bActive) -> bool override;
		LEVELEDITOR_API auto ResetLayout() -> void override;

	protected:
		auto FocusEditorView() -> void override;
		auto PostInitAssetEditor() -> void override;

	private:
		friend class ::Durin::FLevelEditorModule;

		auto InitializeContext() -> void;
		auto PruneAndSaveWorkspaceSettings() -> void;
		auto CreatePanels() -> void;
		auto CreateDocumentControllers() -> void;
		auto CreateImportDialogs() -> void;
		auto RequestContentBrowserImport(
			std::string_view Directory, EImportDialogType Type) -> void;
		auto DrawContentBrowserImport(
			EImportDialogType Type, bool bAllowAssetMutation) -> void;
		auto DrawProjectSettings(std::string& OutError) -> void;
		auto DrawViewportPreferences(std::string& OutError) -> void;
		auto ResetViewportPreferences() -> bool;
		auto LoadProjectSettings() -> bool;
		auto SaveProjectSettings() -> bool;
		auto ApplyFixedUpDefaultLevelPath(const FPackagePath& Path) -> void;
		auto RequestOpenProject(std::string ProjectFile) -> void;
		auto SetError(std::string Message) -> void;
		auto StartPlay(EPlayStartLocation StartLocation, EPlayDestination Destination) -> void;
		auto ApplyPlayChanges(bool bSelectedOnly) -> void;
		auto BuildDefaultLayout(uint32 DockSpaceId, float DockSpaceWidth, float DockSpaceHeight) -> void;

		TWeakObjectPtr<DEditorEngine> EditorOwner;
		std::unique_ptr<FLevelEditorContext> Context;
		// Module-owned services outlive this registered workspace.
		std::shared_ptr<FLevelEditorWorkspaceSettings> WorkspaceSettingsOwner;
		FLevelEditorWorkspaceSettings& WorkspaceSettings;
		FContentBrowserCallbacks ContentBrowserCallbacks;
		std::function<void()> OpenProjectBrowser;
		std::unique_ptr<FLevelDocumentController> DocumentController;
		std::unique_ptr<FEditorAssetMoveCoordinator> AssetMoveCoordinator;
		std::unique_ptr<FSceneImportDialog> SceneImportDialog;
		std::vector<std::unique_ptr<ILevelEditorPanel>> Panels;
		// Panel pointers are non-owning aliases into the Panels collection.
		FSceneViewportPanel* SceneViewportPanel = nullptr;
		FRenderingDiagnosticsPanel* RenderingDiagnosticsPanel = nullptr;
		FDetailsPanel* DetailsPanel = nullptr;
		FEditorTabWindow RootWindow;
		bool bResetLayoutRequested = false;
		bool bWasActive = false;
		FEditorSettingsPageRegistration ViewportSettingsPage;
		FEditorSettingsPageRegistration ProjectSettingsPage;
		TSoftObjectPtr<DLevel> DefaultLevel;
		TSoftObjectPtr<DLevel> PendingDefaultLevel;
		std::string EditorError;
		bool bReleased = false;
	};
} // namespace Durin::LevelEditor
