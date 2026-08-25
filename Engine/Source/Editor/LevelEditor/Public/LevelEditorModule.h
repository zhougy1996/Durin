#pragma once

#include "LevelEditorAPI.h"
#include "Modules/ModuleManager.h"
#include "LevelEditorCustomizations.h"
#include "LevelEditorViewportEditing.h"
#include "LevelEditorContentBrowserCallbacks.h"
#include "ContentBrowser/ContentBrowserContracts.h"

namespace Durin
{
	class FAssetPath;
}

namespace Durin::Asset { class IAssetReferenceStore; }

namespace Durin::Editor
{
	class FWorkspaceRegistrationHandle;
	class FWorkspaceManager;
	class FRenderedAssetThumbnailService;
	class FAssetThumbnailProviderRegistrationHandle;
}

namespace Durin::Editor::Level
{
	class FLevelEditorSessionSettings;
	class MLevelEditor;
}

namespace Durin
{
	// Registers the level workspace and its editor customizations.
	class FLevelEditorModule final : public IModuleInterface
	{
	public:
		LEVELEDITOR_API ~FLevelEditorModule() override;
		LEVELEDITOR_API auto StartupModule() -> void override;
		LEVELEDITOR_API auto ShutdownModule() -> void override;
		LEVELEDITOR_API auto RegisterLevelEditorWorkspace(::Durin::Editor::FWorkspaceManager& WorkspaceManager,
			::Durin::Editor::FRenderedAssetThumbnailService& ThumbnailService,
			std::function<void(AssetForge::FImportOperationHandle, std::string)>
				NotifyImportStarted,
			Editor::Level::FContentBrowserCallbacks ContentBrowserCallbacks) -> bool;
		LEVELEDITOR_API auto UnregisterLevelEditorWorkspace() -> void;
		LEVELEDITOR_API auto OpenDefaultDocument() -> bool;
		LEVELEDITOR_API auto OpenImportDialog(
			std::string Directory,
			Editor::Level::EImportDialogType Type) -> void;
	private:
		FModuleOwnedCallbackRegistration EditorExtensionCallbacks;
		FAsyncOperationGroup ThumbnailOperations;
		std::unique_ptr<::Durin::Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<Editor::Level::FLevelEditorSessionSettings> SessionSettings;
		std::vector<Editor::Level::FLevelEditorCustomizationHandle> CustomizationHandles;
		Editor::Level::FLevelViewportEditModeHandle SplineEditModeHandle;
		std::weak_ptr<Editor::Level::MLevelEditor> LevelEditorWorkspace;
		std::unique_ptr<::Durin::Asset::IAssetReferenceStore>
			ProjectDefaultLevelReferenceStore;
		uint64 ProjectDefaultLevelReferenceStoreHandle = 0;
		uint64 GrayboxBuildStartupCommandHandle = 0;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle>
			TerrainThumbnailRegistration;
		std::vector<Editor::ContentBrowser::FScopedExtensionRegistration>
			ContentBrowserExtensions;
	};
}
