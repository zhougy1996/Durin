#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"
#include "Documents/EditorDocumentTypes.h"
#include "MonaImGui.h"

namespace Durin
{
	class FEditorDocumentManager;
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
		bool bDockInEditorHost = true;
		bool bNativeStandalone = false;
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

	// Presents document tabs for one toolkit; it owns no assets or document lifetime.
	// Focus and close intentions are submitted to FEditorDocumentManager.
	class FTabManager
	{
	public:
		DURINED_API auto RequestFocus(FDocumentId DocumentId) -> void;
		// Draws every document of one toolkit and defers manager mutation until iteration completes.
		DURINED_API auto DrawDocuments(
			FEditorDocumentManager& DocumentManager,
			const FAssetEditorToolkitId& ToolkitId,
			std::string_view WorkspaceRootKey,
			const std::function<bool(const FEditorDocument&)>& CanDrawDocument,
			const std::function<void(const FEditorDocument&)>& DrawDocument,
			const std::function<void(const FEditorDocument&, bool)>& UpdateDocumentVisibility = {}
		) -> bool;

	private:
		std::unordered_map<uint64, FEditorTabWindow> DocumentWindows;
		FEditorTabWindow SessionWindow;
		bool bSessionFocusRequested = false;
		FDocumentId FocusDocument;
	};

}
