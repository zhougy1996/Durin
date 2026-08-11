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
	namespace Editor
	{
		class FWorkspaceRegistrationHandle;
		class FWorkspaceManager;
	}

	// Registers the level workspace and its editor customizations.
	class FLevelEditorModule final : public IModuleInterface
	{
	public:
		LEVELEDITOR_API ~FLevelEditorModule() override;
		LEVELEDITOR_API auto StartupModule() -> void override;
		LEVELEDITOR_API auto ShutdownModule() -> void override;
		LEVELEDITOR_API auto RegisterLevelEditorWorkspace(Editor::FWorkspaceManager& WorkspaceManager) -> bool;
		LEVELEDITOR_API auto UnregisterLevelEditorWorkspace() -> void;
		LEVELEDITOR_API auto OpenDefaultDocument() -> bool;
		LEVELEDITOR_API auto RevealAssetInContentBrowser(const FAssetPath& AssetPath) -> bool;
	private:
		std::unique_ptr<Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<FLevelEditorSessionSettings> SessionSettings;
		std::vector<FLevelEditorCustomizationHandle> CustomizationHandles;
		FLevelViewportEditModeHandle SplineEditModeHandle;
		std::weak_ptr<class MLevelEditor> LevelEditorWorkspace;
		std::unique_ptr<Asset::IAssetReferenceStore>
			ProjectDefaultLevelReferenceStore;
		uint64 ProjectDefaultLevelReferenceStoreHandle = 0;
		uint64 GrayboxBuildStartupCommandHandle = 0;
	};
}
