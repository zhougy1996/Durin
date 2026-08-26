#pragma once

#include "Modules/ModuleManager.h"
#include "StaticMeshEditorAPI.h"
#include "ContentBrowser/ContentBrowserContracts.h"
#include "Editor/Import/ImportDialogSupport.h"

namespace Durin::Editor
{
	class FWorkspaceManager;
	class FWorkspaceRegistrationHandle;
	class FAssetThumbnailProviderRegistry;
	class FAssetThumbnailProviderRegistrationHandle;
}

namespace Durin::Editor::StaticMesh { class FStaticMeshImportDialog; }

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
			::Durin::Editor::FAssetThumbnailProviderRegistry& ThumbnailService,
			::Durin::Editor::Import::FImportDialogCallbacks ImportCallbacks = {}) -> bool;
		STATICMESHEDITOR_API auto UnregisterStaticMeshEditor() -> void;
		STATICMESHEDITOR_API auto DrawImportDialogs() -> void;

	private:
		FModuleOwnedCallbackRegistration EditorExtensionCallbacks;
		std::unique_ptr<::Durin::Editor::FWorkspaceRegistrationHandle> WorkspaceRegistration;
		std::unique_ptr<::Durin::Editor::FAssetThumbnailProviderRegistrationHandle> ThumbnailRegistration;
		Editor::ContentBrowser::FScopedExtensionRegistration
			ContentBrowserImportExtension;
		Editor::ContentBrowser::FScopedExtensionRegistration
			ContentBrowserReimportExtension;
		std::unique_ptr<Editor::StaticMesh::FStaticMeshImportDialog> ImportDialog;
	};
}
