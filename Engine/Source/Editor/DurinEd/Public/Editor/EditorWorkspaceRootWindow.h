#pragma once

#include "DurinEdAPI.h"
#include "Editor/EditorWorkspace.h"
#include "MonaImGui.h"

namespace Durin
{
	// Describes an optional dock space nested inside a workspace root window.
	struct FEditorWorkspaceInternalDockSpace
	{
		FEditorWorkspaceTypeId WorkspaceType;
		uint32 LayoutVersion = 0;
	};

	// Configures one frame of an editor workspace root window.
	struct FEditorWorkspaceRootWindowConfig
	{
		std::string_view DisplayName;
		std::string_view RootKey;
		bool bDirty = false;
		bool bZeroPadding = false;
		bool bDockInEditorHost = true;
		ImGuiWindowFlags AdditionalFlags = ImGuiWindowFlags_None;
		std::optional<FEditorWorkspaceInternalDockSpace> InternalDockSpace;
	};

	// Reports visibility, activation, focus, and close intent for one frame.
	struct FEditorWorkspaceRootWindowState
	{
		bool bVisible = false;
		bool bFocused = false;
		bool bActivated = false;
		bool bCloseRequested = false;
	};

	// Balances ImGui root-window lifetime and tracks dock-tab activation.
	class FEditorWorkspaceRootWindow
	{
	public:
		auto RequestFocus() -> void { bFocusRequested = true; }
		auto ResetActivationState() -> void { bWasDockTabSelected = false; }
		DURINED_API auto Begin(const FEditorWorkspaceRootWindowConfig& Config) -> FEditorWorkspaceRootWindowState;
		DURINED_API auto End() -> void;

	private:
		bool bFocusRequested = false;
		bool bWasDockTabSelected = false;
		bool bWindowBegun = false;
	};

	// Hosts the repeated root-window lifecycle for one per-resource workspace.
	class FEditorWorkspaceDocumentHost
	{
	public:
		DURINED_API auto RequestFocus(FEditorDocumentId DocumentId) -> void;
		// Draws every document of one workspace and defers manager mutation until iteration completes.
		DURINED_API auto DrawDocuments(
			FEditorWorkspaceManager& WorkspaceManager,
			const FEditorWorkspaceTypeId& WorkspaceType,
			std::string_view WorkspaceRootKey,
			const std::function<bool(const FEditorDocumentTab&)>& CanDrawDocument,
			const std::function<void(const FEditorDocumentTab&)>& DrawDocument,
			const std::function<void(const FEditorDocumentTab&)>& PrepareDocument = {}
		) -> bool;

	private:
		std::unordered_map<uint64, FEditorWorkspaceRootWindow> DocumentWindows;
	};
}
