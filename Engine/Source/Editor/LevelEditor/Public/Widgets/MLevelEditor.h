#pragma once

#include "LevelEditorAPI.h"
#include "LevelEditorContentBrowserCallbacks.h"
#include "DObject/SoftObjectPtr.h"
#include "Editor/Workspace.h"
#include "Editor/WorkspaceRootWindow.h"
#include "Modules/ModularFeature.h"
#include "Threading/Task.h"
#include "AssetForge/Operations/ImportOperation.h"

namespace Durin
{
	class DLevel;
	class FAssetPath;
	class FLevelEditorModule;
}

namespace Durin::Editor
{
	enum class EPlayStartLocation : uint8;
	enum class EPlayDestination : uint8;
	class FWorkspaceManager;
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
	class FTerrainHeightmapImportDialog;
	class FDetailsPanel;
	struct FLevelEditorContext;

	// Hosts level documents, panels, play controls, and project settings.
	class MLevelEditor final : public ::Durin::Editor::IWorkspace
	{
	public:
		MLevelEditor(FLevelEditorSessionSettings& InSessionSettings,
			::Durin::Editor::FWorkspaceManager& InWorkspaceManager,
			FModuleOwnedCallbackGate InOwnerGate,
			FTaskScopeToken InThumbnailTaskScope,
			std::function<void(AssetForge::FImportOperationHandle, std::string)>
				InNotifyImportStarted,
			FContentBrowserCallbacks InContentBrowserCallbacks);
		LEVELEDITOR_API ~MLevelEditor() override;
		LEVELEDITOR_API auto Construct() -> void;
		LEVELEDITOR_API auto OpenDefaultDocument() -> bool;
		LEVELEDITOR_API auto GetWorkspaceType() const -> const ::Durin::Editor::FWorkspaceTypeId& override;
		LEVELEDITOR_API auto OpenDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentOpenResult override;
		LEVELEDITOR_API auto ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void override;
		LEVELEDITOR_API auto RequestDeactivate() -> bool override;
		LEVELEDITOR_API auto RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentCloseResult override;
		LEVELEDITOR_API auto SaveDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool override;
		LEVELEDITOR_API auto DiscardDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool override;
		LEVELEDITOR_API auto IsDocumentDirty(const ::Durin::Editor::FDocumentTab& Document) const -> bool override;
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
		LEVELEDITOR_API auto RequestContentBrowserImport(
			const std::string& Directory,
			EImportDialogType Type) -> void;

	private:
		friend class ::Durin::FLevelEditorModule;

		auto InitializeContext() -> void;
		auto InitializeSession() -> void;
		auto CreatePanels() -> void;
		auto CreateDocumentServices() -> void;
		auto CreateImportDialogs() -> void;
		auto FinalizeSessionConstruction() -> void;
		auto DrawProjectSettings() -> void;
		auto LoadProjectSettings() -> bool;
		auto SaveProjectSettings() -> bool;
		auto ApplyFixedUpDefaultLevelPath(const FAssetPath& Path) -> void;
		auto SetError(std::string Message) -> void;
		auto StartPlay(::Durin::Editor::EPlayStartLocation StartLocation, ::Durin::Editor::EPlayDestination Destination) -> void;
		auto ApplyPlayChanges(bool bSelectedOnly) -> void;
		auto BuildDefaultLayout(uint32 DockSpaceId, float DockSpaceWidth, float DockSpaceHeight) -> void;

		std::unique_ptr<FLevelEditorContext> Context;
		// Module-owned services outlive this registered workspace.
		FLevelEditorSessionSettings& SessionSettings;
		::Durin::Editor::FWorkspaceManager& WorkspaceManager;
		FModuleOwnedCallbackGate OwnerGate;
		FTaskScopeToken ThumbnailTaskScope;
		std::function<void(AssetForge::FImportOperationHandle, std::string)>
			NotifyImportStarted;
		FContentBrowserCallbacks ContentBrowserCallbacks;
		std::unique_ptr<FLevelDocumentController> DocumentController;
		std::unique_ptr<FEditorAssetMoveCoordinator> AssetMoveCoordinator;
		std::unique_ptr<FSceneImportDialog> SceneImportDialog;
		std::unique_ptr<FTerrainHeightmapImportDialog> TerrainHeightmapImportDialog;
		std::vector<std::unique_ptr<ILevelEditorPanel>> Panels;
		// Panel pointers are non-owning aliases into the Panels collection.
		FSceneViewportPanel* SceneViewportPanel = nullptr;
		FRenderingDiagnosticsPanel* RenderingDiagnosticsPanel = nullptr;
		FDetailsPanel* DetailsPanel = nullptr;
		::Durin::Editor::FWorkspaceRootWindow RootWindow;
		bool bResetLayoutRequested = false;
		bool bWasActive = false;
		bool bProjectSettingsOpen = false;
		TSoftObjectPtr<DLevel> DefaultLevel;
		TSoftObjectPtr<DLevel> PendingDefaultLevel;
		std::string EditorError;
		::Durin::Editor::FDocumentId DeferredOpenDocumentId;
	};
} // namespace Durin::Editor::Level
