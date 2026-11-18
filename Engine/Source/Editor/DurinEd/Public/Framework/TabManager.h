#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"
#include "DObject/ObjectKey.h"
#include "AssetDefinition/AssetOpenTypes.h"
#include "Toolkits/AssetEditorTypes.h"
#include "MonaImGui.h"

namespace Durin
{
	class DObject;
	class FAssetEditorToolkit;
	// Describes an optional dock space nested inside a toolkit root window.
	struct FToolkitInternalDockSpace
	{
		FAssetEditorToolkitId ToolkitId;
		uint32 LayoutVersion = 0;
	};

	// Configures one frame of an editor toolkit root window.
	struct FEditorTabWindowConfig
	{
		std::string_view DisplayName;
		std::string_view RootKey;
		bool bDirty = false;
		bool bZeroPadding = false;
		EAssetEditorOpenLocation OpenLocation = EAssetEditorOpenLocation::MainWindow;
		ImGuiWindowFlags AdditionalFlags = ImGuiWindowFlags_None;
		std::optional<FToolkitInternalDockSpace> InternalDockSpace;
	};

	// Reports visibility, activation, focus, and close intent for one frame.
	struct FEditorTabWindowState
	{
		bool bVisible = false;
		bool bFocused = false;
		bool bActivated = false;
		bool bCloseRequested = false;
	};

	// Balances ImGui root-window lifetime and tracks dock-tab activation.
	class FEditorTabWindow
	{
	public:
		auto RequestFocus() -> void { bFocusRequested = true; }
		auto ResetActivationState() -> void { bWasDockTabSelected = false; }
		DURINED_API auto Begin(const FEditorTabWindowConfig& Config) -> FEditorTabWindowState;
		DURINED_API auto End() -> void;

	private:
		bool bFocusRequested = false;
		bool bWasDockTabSelected = false;
		bool bWindowBegun = false;
	};

	// Presents the Toolkit's assets and forwards focus and close requests.
	class FTabManager
	{
	public:
		DURINED_API auto RequestFocus(DObject& Asset) -> void;
		auto SelectAsset(FObjectKey Asset) noexcept -> void { FocusedAsset = Asset; }
		DURINED_API auto DrawAssets(FAssetEditorToolkit& Toolkit, std::string_view RootKey,
			const std::function<void(DObject&)>& DrawAsset,
			const std::function<void(DObject&, bool)>& UpdateVisibility) -> bool;

	private:
		FEditorTabWindow SessionWindow;
		FObjectKey FocusedAsset;
	};
}
