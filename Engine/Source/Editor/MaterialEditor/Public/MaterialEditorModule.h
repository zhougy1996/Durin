#pragma once

#include "MaterialEditorAPI.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	namespace Editor
	{
		class FWorkspaceRegistrationHandle;
		class FWorkspaceManager;
	}
	class FRenderedAssetThumbnailService;
	class FAssetThumbnailProviderRegistrationHandle;

	// Registers the material workspace and material asset-editor mapping.
	class FMaterialEditorModule final : public IModuleInterface
	{
	public:
		MATERIALEDITOR_API ~FMaterialEditorModule() override;
		MATERIALEDITOR_API auto StartupModule() -> void override;
		MATERIALEDITOR_API auto ShutdownModule() -> void override;
		MATERIALEDITOR_API auto RegisterMaterialEditor(
			Editor::FWorkspaceManager& WorkspaceManager,
			FRenderedAssetThumbnailService& ThumbnailService) -> bool;
		MATERIALEDITOR_API auto UnregisterMaterialEditor() -> void;

	private:
		std::unique_ptr<Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<FAssetThumbnailProviderRegistrationHandle> MaterialThumbnailRegistration;
		std::unique_ptr<FAssetThumbnailProviderRegistrationHandle> MaterialInstanceThumbnailRegistration;
	};
}
