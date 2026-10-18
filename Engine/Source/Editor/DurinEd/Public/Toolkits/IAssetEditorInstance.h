#pragma once

#include "CoreMinimal.h"
#include "Misc/Name.h"

namespace Durin
{
	class DObject;

	// Preserves the request origin independently of a document's save/close policy.
	enum class EAssetEditorCloseReason : uint8
	{
		CloseAllEditorsForAsset,
		CloseOtherEditors,
		RemoveAssetFromAllEditors,
		CloseAllAssetEditors,
		AssetEditorHostClosed,
		AssetUnloadingOrInvalid,
		EditorRefreshRequested,
		AssetForceDeleted,
	};

	// Native editor instance contract shared by the asset service and document hosts.
	class IAssetEditorInstance
	{
	public:
		virtual ~IAssetEditorInstance() = default;
		virtual auto GetEditorName() const -> FName = 0;
		virtual auto IsPrimaryEditor() const -> bool { return true; }
		virtual auto FocusWindow(DObject* ObjectToFocusOn = nullptr) -> void = 0;
		virtual auto GetLastActivationTime() -> double = 0;
		// False includes a deferred dirty-document confirmation; never implies discard.
		virtual auto CloseWindow(EAssetEditorCloseReason InCloseReason) -> bool = 0;
		virtual auto CloseWindow() -> bool { return CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed); }
	};
}
