#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Import/ImportDialogSupport.h"

namespace Durin::Editor
{
	class FEditorDocumentManager;
	class DThumbnailManager;

	// Fixed module composition uses FModuleManager; modules own their scoped handles.
	class IAssetEditorModule : public IModuleInterface
	{
	public:
		virtual auto RegisterToolkits(FEditorDocumentManager& Manager, DThumbnailManager& Thumbnails,
			FImportDialogCallbacks ImportCallbacks = {}) -> bool = 0;
		virtual auto UnregisterToolkits() -> void = 0;
	};
}
