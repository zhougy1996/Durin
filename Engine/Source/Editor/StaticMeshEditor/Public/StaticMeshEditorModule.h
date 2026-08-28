#pragma once

#include "Modules/ModuleManager.h"
#include "StaticMeshEditorAPI.h"
#include "Editor/Import/ImportDialogSupport.h"

namespace Durin::Editor
{
	class FWorkspaceManager;
	class FWorkspaceRegistrationHandle;
	class DThumbnailManager;
	class FThumbnailRendererRegistrationHandle;
}

namespace Durin
{
	// Registers the read-only StaticMesh inspector and its exact asset route.
	class FStaticMeshEditorModule final : public IModuleInterface
	{
	public:
		STATICMESHEDITOR_API FStaticMeshEditorModule();
		STATICMESHEDITOR_API ~FStaticMeshEditorModule() override;
		STATICMESHEDITOR_API auto StartupModule() -> void override;
		STATICMESHEDITOR_API auto ShutdownModule() -> void override;
		STATICMESHEDITOR_API auto RegisterStaticMeshEditor(
			::Durin::Editor::FWorkspaceManager& WorkspaceManager,
			::Durin::Editor::DThumbnailManager& ThumbnailManager,
			::Durin::Editor::FImportDialogCallbacks ImportCallbacks = {}) -> bool;
		STATICMESHEDITOR_API auto UnregisterStaticMeshEditor() -> void;
		STATICMESHEDITOR_API auto OpenImportDialog(std::string_view Directory) -> void;
		STATICMESHEDITOR_API auto DrawImportDialog(bool bAllowAssetMutation) -> void;
	private:
		FModuleOwnedCallbackRegistration EditorExtensionCallbacks;
		std::unique_ptr<::Durin::Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<::Durin::Editor::FThumbnailRendererRegistrationHandle> ThumbnailRegistration;
		struct FIntegrationState;
		std::unique_ptr<FIntegrationState> Integration;
	};
}
