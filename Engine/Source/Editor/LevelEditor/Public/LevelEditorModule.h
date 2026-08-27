#pragma once

#include "LevelEditorAPI.h"
#include "Modules/ModuleManager.h"
#include "LevelEditorCustomizations.h"
#include "LevelEditorViewportEditing.h"
#include "LevelEditorContentBrowserCallbacks.h"

namespace Durin
{
	class FAssetPath;
}

namespace Durin::Asset { class IAssetReferenceStore; }

namespace Durin::Editor
{
	class FWorkspaceRegistrationHandle;
	class FWorkspaceManager;
	class FAssetThumbnailProviderRegistry;
	class FAssetThumbnailProviderRegistrationHandle;
}

namespace Durin::Editor::Level
{
	enum class EImportDialogType : uint8;
	class FLevelEditorSessionSettings;
	class MLevelEditor;
}

namespace Durin
{
	// Registers the level workspace and its editor customizations.
	class FLevelEditorModule final : public IModuleInterface
	{
	public:
		LEVELEDITOR_API FLevelEditorModule();
		LEVELEDITOR_API ~FLevelEditorModule() override;
		LEVELEDITOR_API auto StartupModule() -> void override;
		LEVELEDITOR_API auto ShutdownModule() -> void override;
		LEVELEDITOR_API auto RegisterLevelEditorWorkspace(::Durin::Editor::FWorkspaceManager& WorkspaceManager,
			::Durin::Editor::FAssetThumbnailProviderRegistry& ThumbnailService,
			Editor::Level::FContentBrowserCallbacks ContentBrowserCallbacks) -> bool;
		LEVELEDITOR_API auto UnregisterLevelEditorWorkspace() -> void;
		LEVELEDITOR_API auto OpenDefaultDocument() -> bool;
		LEVELEDITOR_API auto OpenImportDialog(
			Editor::Level::EImportDialogType Type, std::string_view Directory) -> void;
		LEVELEDITOR_API auto ReimportTerrainHeightmap(std::string_view AssetPath,
			std::function<void(std::string)> ReportError) -> void;
		LEVELEDITOR_API auto ReimportTerrainHeightmapFromFile(std::string_view AssetPath,
			std::function<void(std::string)> ReportError) -> void;
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
		struct FIntegrationState;
		std::unique_ptr<FIntegrationState> Integration;
	};
}
