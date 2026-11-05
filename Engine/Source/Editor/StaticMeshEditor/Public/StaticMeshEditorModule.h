#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorModule.h"

#include "Modules/ModuleManager.h"
#include "StaticMeshEditorAPI.h"
#include "Import/ImportDialogSupport.h"

namespace Durin::Editor
{
	class FEditorDocumentManager;
	class FAssetEditorToolkitRegistrationHandle;
	class DThumbnailManager;
	class FThumbnailRendererRegistrationHandle;
}

namespace Durin
{
	// Registers the read-only StaticMesh inspector and its exact asset route.
	class FStaticMeshEditorModule final : public Editor::IAssetEditorModule
	{
	public:
		STATICMESHEDITOR_API FStaticMeshEditorModule();
		STATICMESHEDITOR_API ~FStaticMeshEditorModule() override;
		STATICMESHEDITOR_API auto StartupModule() -> void override;
		STATICMESHEDITOR_API auto ShutdownModule() -> void override;
		STATICMESHEDITOR_API auto RegisterToolkits(
			Editor::FEditorDocumentManager& DocumentManager,
			Editor::DThumbnailManager& ThumbnailManager,
			Editor::FImportDialogCallbacks ImportCallbacks = {}) -> bool override;
		STATICMESHEDITOR_API auto UnregisterToolkits() -> void override;
	private:
		std::unique_ptr<Editor::FAssetEditorToolkitRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<Editor::FThumbnailRendererRegistrationHandle> ThumbnailRegistration;
		struct FIntegrationState;
		std::unique_ptr<FIntegrationState> Integration;
	};
}
