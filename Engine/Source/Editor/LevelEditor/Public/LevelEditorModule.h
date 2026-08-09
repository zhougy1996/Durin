#pragma once

#include "LevelEditorAPI.h"
#include "Modules/ModuleManager.h"
#include "LevelEditorCustomizations.h"
#include "LevelEditorViewportEditing.h"

namespace Durin
{
	namespace Asset
	{
		class IAssetReferenceStore;
	}

	class FLevelEditorSessionSettings;
	class FAssetPath;
	class FEditorWorkspaceRegistrationHandle;
	class FEditorWorkspaceManager;

	// Registers the level workspace and its editor customizations.
	class FLevelEditorModule final : public IModuleInterface
	{
	public:
		LEVELEDITOR_API ~FLevelEditorModule() override;
		LEVELEDITOR_API auto StartupModule() -> void override;
		LEVELEDITOR_API auto ShutdownModule() -> void override;
		LEVELEDITOR_API auto RegisterLevelEditorWorkspace(FEditorWorkspaceManager& WorkspaceManager) -> bool;
		LEVELEDITOR_API auto UnregisterLevelEditorWorkspace() -> void;
		LEVELEDITOR_API auto OpenDefaultDocument() -> bool;
		LEVELEDITOR_API auto RevealAssetInContentBrowser(const FAssetPath& AssetPath) -> bool;
	private:
		std::unique_ptr<FEditorWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<FLevelEditorSessionSettings> SessionSettings;
		std::vector<FLevelEditorCustomizationHandle> CustomizationHandles;
		FLevelViewportEditModeHandle SplineEditModeHandle;
		std::weak_ptr<class MLevelEditor> LevelEditorWorkspace;
		std::unique_ptr<Asset::IAssetReferenceStore>
			ProjectDefaultLevelReferenceStore;
		uint64 ProjectDefaultLevelReferenceStoreHandle = 0;
	};
}
