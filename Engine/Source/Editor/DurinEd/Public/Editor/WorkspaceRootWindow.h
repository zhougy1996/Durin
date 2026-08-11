#pragma once

#include "DurinEdAPI.h"
#include "Editor/WorkspaceTypes.h"
#include "MonaImGui.h"

namespace Durin::Editor
{
	class FWorkspaceManager;
	// Describes an optional dock space nested inside a workspace root window.
	struct FWorkspaceInternalDockSpace
	{
		FWorkspaceTypeId WorkspaceType;
		uint32 LayoutVersion = 0;
	};

	// Configures one frame of an editor workspace root window.
	struct FWorkspaceRootWindowConfig
	{
		std::string_view DisplayName;
		std::string_view RootKey;
		bool bDirty = false;
		bool bZeroPadding = false;
		bool bDockInEditorHost = true;
		ImGuiWindowFlags AdditionalFlags = ImGuiWindowFlags_None;
		std::optional<FWorkspaceInternalDockSpace> InternalDockSpace;
	};

	// Reports visibility, activation, focus, and close intent for one frame.
	struct FWorkspaceRootWindowState
	{
		bool bVisible = false;
		bool bFocused = false;
		bool bActivated = false;
		bool bCloseRequested = false;
	};

	// Balances ImGui root-window lifetime and tracks dock-tab activation.
	class FWorkspaceRootWindow
	{
	public:
		auto RequestFocus() -> void { bFocusRequested = true; }
		auto ResetActivationState() -> void { bWasDockTabSelected = false; }
		DURINED_API auto Begin(const FWorkspaceRootWindowConfig& Config) -> FWorkspaceRootWindowState;
		DURINED_API auto End() -> void;

	private:
		bool bFocusRequested = false;
		bool bWasDockTabSelected = false;
		bool bWindowBegun = false;
	};

	// Hosts the repeated root-window lifecycle for one per-resource workspace.
	class FWorkspaceDocumentHost
	{
	public:
		DURINED_API auto RequestFocus(FDocumentId DocumentId) -> void;
		// Draws every document of one workspace and defers manager mutation until iteration completes.
		DURINED_API auto DrawDocuments(
			FWorkspaceManager& WorkspaceManager,
			const FWorkspaceTypeId& WorkspaceType,
			std::string_view WorkspaceRootKey,
			const std::function<bool(const FDocumentTab&)>& CanDrawDocument,
			const std::function<void(const FDocumentTab&)>& DrawDocument,
			const std::function<void(const FDocumentTab&)>& PrepareDocument = {}
		) -> bool;

	private:
		std::unordered_map<uint64, FWorkspaceRootWindow> DocumentWindows;
	};
}
