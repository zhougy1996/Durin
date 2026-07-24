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
}
