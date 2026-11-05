#pragma once

#include "CoreMinimal.h"
#include "Engine/HostStartup.h"

#include "LevelEditorAPI.h"
#include "LevelEditorContentBrowserCallbacks.h"
#include "DObject/SoftObjectPtr.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/AssetEditorDocumentModels.h"
#include "Settings/EditorSettings.h"

namespace Durin
{
	class DLevel;
	class FPackagePath;
	class FLevelEditorModule;
}

namespace Durin::Editor
{
	enum class EPlayStartLocation : uint8;
	enum class EPlayDestination : uint8;
	class FEditorDocumentManager;
}

namespace Durin::Editor::Level
{
	class ILevelEditorPanel;
	class FLevelEditorSessionSettings;
	class FEditorAssetMoveCoordinator;
	class FLevelDocumentController;
	class FSceneViewportPanel;
	class FRenderingDiagnosticsPanel;
	class FSceneImportDialog;
	class FDetailsPanel;
	struct FLevelEditorContext;

	// Hosts level documents, panels, play controls, and project settings.
	class MLevelEditor final : public IAssetEditorToolkit
	{
	public:
		MLevelEditor(FLevelEditorSessionSettings& InSessionSettings,
			FEditorDocumentManager& InWorkspaceManager,
			FContentBrowserCallbacks InContentBrowserCallbacks,
			std::function<void()> InOpenProjectBrowser);
		LEVELEDITOR_API ~MLevelEditor() override;
		LEVELEDITOR_API auto Construct() -> void;
		LEVELEDITOR_API auto GetToolkitId() const -> const FAssetEditorToolkitId& override;
		LEVELEDITOR_API auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult override;
		LEVELEDITOR_API auto ActivateDocument(const FEditorDocument& Document) -> void override;
		LEVELEDITOR_API auto RequestDeactivate() -> bool override;
		LEVELEDITOR_API auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult override;
		LEVELEDITOR_API auto SaveDocument(const FEditorDocument& Document) -> bool override;
		LEVELEDITOR_API auto DiscardDocument(const FEditorDocument& Document) -> bool override;
		LEVELEDITOR_API auto IsDocumentDirty(const FEditorDocument& Document) const -> bool override;
		LEVELEDITOR_API auto CanSaveActiveDocument() const -> bool override;
		LEVELEDITOR_API auto SaveActiveDocument() -> bool override;
		LEVELEDITOR_API auto CanUndo() const -> bool override;
		LEVELEDITOR_API auto CanRedo() const -> bool override;
		LEVELEDITOR_API auto GetUndoDescription() const -> std::string override;
		LEVELEDITOR_API auto GetRedoDescription() const -> std::string override;
		LEVELEDITOR_API auto Undo() -> bool override;
		LEVELEDITOR_API auto Redo() -> bool override;
		LEVELEDITOR_API auto DrawFileMenu() -> void override;
		LEVELEDITOR_API auto DrawEditMenu() -> void override;
		LEVELEDITOR_API auto DrawWindowMenu() -> void override;
		LEVELEDITOR_API auto DrawToolkit(bool bActive) -> bool override;
		LEVELEDITOR_API auto ResetLayout() -> void override;

	private:
		friend class ::Durin::FLevelEditorModule;

		auto InitializeContext() -> void;
		auto InitializeSession() -> void;
		auto CreatePanels() -> void;
		auto CreateDocumentServices() -> void;
		auto CreateImportDialogs() -> void;
		auto RequestContentBrowserImport(
			std::string_view Directory, EImportDialogType Type) -> void;
		auto DrawContentBrowserImport(
			EImportDialogType Type, bool bAllowAssetMutation) -> void;
		auto FinalizeSessionConstruction() -> void;
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

		std::unique_ptr<FLevelEditorContext> Context;
		// Module-owned services outlive this registered workspace.
		FLevelEditorSessionSettings& SessionSettings;
		FEditorDocumentManager& DocumentManager;
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
		FDocumentId DeferredOpenDocumentId;
	};
} // namespace Durin::Editor::Level
