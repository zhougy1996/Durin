#pragma once

#include "CoreMinimal.h"

#include "LevelEditorAPI.h"
#include "Modules/ModuleManager.h"
#include "LevelEditorCustomizations.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"
#include "LevelEditorViewportEditing.h"
#include "LevelEditorContentBrowserCallbacks.h"

namespace Durin
{
	class FPackagePath;
}

namespace Durin { class IAssetReferenceStore; }

namespace Durin { class DAssetEditorSubsystem; class DAssetEditor; class FAssetEditorToolkit; }
namespace Durin::Editor
{
	class DThumbnailManager;
	class FThumbnailRendererRegistrationHandle;

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
		LEVELEDITOR_API FLevelEditorModule();
		LEVELEDITOR_API ~FLevelEditorModule() override;
		LEVELEDITOR_API auto StartupModule() -> void override;
		LEVELEDITOR_API auto CanShutdownModule() const -> bool override;
		LEVELEDITOR_API auto ShutdownModule() -> void override;
		LEVELEDITOR_API auto InitializeLevelEditor(DAssetEditorSubsystem& AssetEditors,
			Editor::Level::FContentBrowserCallbacks ContentBrowserCallbacks,
			std::function<void()> OpenProjectBrowser) -> bool;
		LEVELEDITOR_API auto ReleaseLevelEditor(DAssetEditorSubsystem& AssetEditors) -> void;
		LEVELEDITOR_API auto GetOrCreateLevelEditor(DAssetEditorSubsystem& AssetEditors) -> DAssetEditor*;
		LEVELEDITOR_API auto RequestOpenProject(DAssetEditorSubsystem& AssetEditors, std::string ProjectFile) -> bool;
	private:
		std::vector<Editor::Level::FLevelEditorCustomizationHandle> CustomizationHandles;
		std::vector<Editor::FDetailCustomizationHandle> DetailCustomizationHandles;
		Editor::Level::FLevelViewportEditModeHandle SplineEditModeHandle;
		std::unique_ptr<IAssetReferenceStore>
			ProjectDefaultLevelReferenceStore;
		uint64 ProjectDefaultLevelReferenceStoreHandle = 0;
		struct FIntegrationState;
		std::unique_ptr<FIntegrationState> Integration;
	};
}
