#pragma once

#include "CoreMinimal.h"
#include "Modules/ModularFeature.h"
#include "Import/ImportDialogSupport.h"

namespace Durin::Editor
{
	class FWorkspaceManager;
	class DThumbnailManager;

	// Modules publish their integration; existing scoped handles own its resources.
	class IWorkspaceProvider : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "EditorWorkspaceProvider";
		static constexpr uint32 FeatureVersion = 1;
		// False leaves no new contribution; unregister is idempotent and also runs on module shutdown.
		virtual auto RegisterWorkspaces(FWorkspaceManager& Manager, DThumbnailManager& Thumbnails,
			FImportDialogCallbacks ImportCallbacks = {}) -> bool = 0;
		virtual auto UnregisterWorkspaces() -> void = 0;
	};
}
