#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorModule.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"

#include "MaterialEditorAPI.h"
#include "Modules/ModuleManager.h"

namespace Durin::Editor
{
	class FAssetEditorToolkitRegistrationHandle;
	class FEditorDocumentManager;
	class DThumbnailManager;
	class FThumbnailRendererRegistrationHandle;
}

namespace Durin
{
	// Registers the material workspace and material asset-editor mapping.
	class FMaterialEditorModule final : public Editor::IAssetEditorModule
	{
	public:
		MATERIALEDITOR_API FMaterialEditorModule();
		MATERIALEDITOR_API ~FMaterialEditorModule() override;
		MATERIALEDITOR_API auto StartupModule() -> void override;
		MATERIALEDITOR_API auto ShutdownModule() -> void override;
		MATERIALEDITOR_API auto RegisterToolkits(
			Editor::FEditorDocumentManager& DocumentManager,
			Editor::DThumbnailManager& ThumbnailManager,
			Editor::FImportDialogCallbacks ImportCallbacks = {}) -> bool override;
		MATERIALEDITOR_API auto UnregisterToolkits() -> void override;

	private:
		Editor::FDetailCustomizationHandle DetailCustomizationRegistration;
		std::unique_ptr<Editor::FAssetEditorToolkitRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<Editor::FThumbnailRendererRegistrationHandle> MaterialThumbnailRegistration;
		std::unique_ptr<Editor::FThumbnailRendererRegistrationHandle> MaterialInstanceThumbnailRegistration;
		struct FIntegrationState;
		std::unique_ptr<FIntegrationState> Integration;
	};
}
