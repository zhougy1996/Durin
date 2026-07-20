#pragma once

#include "DurinEdAPI.h"
#include "Editor/EditorWorkspace.h"
#include "MonaImGui.h"

namespace Durin
{
	struct FEditorWorkspaceInternalDockSpace
	{
		FEditorWorkspaceTypeId WorkspaceType;
		uint32 LayoutVersion = 0;
	};

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

	struct FEditorWorkspaceRootWindowState
	{
		bool bVisible = false;
		bool bFocused = false;
		bool bActivated = false;
		bool bCloseRequested = false;
	};

	class DURINED_API FEditorWorkspaceRootWindow
	{
	public:
		auto RequestFocus() -> void { bFocusRequested = true; }
		auto ResetActivationState() -> void { bWasDockTabSelected = false; }
		auto Begin(const FEditorWorkspaceRootWindowConfig& Config) -> FEditorWorkspaceRootWindowState;
		auto End() -> void;

	private:
		bool bFocusRequested = false;
		bool bWasDockTabSelected = false;
		bool bWindowBegun = false;
	};
}
