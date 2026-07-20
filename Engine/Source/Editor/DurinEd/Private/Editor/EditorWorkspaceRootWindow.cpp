#include "Editor/EditorWorkspaceRootWindow.h"

#include "Editor/EditorWorkspaceUI.h"

namespace Durin
{
	auto FEditorWorkspaceRootWindow::Begin(const FEditorWorkspaceRootWindowConfig& Config) -> FEditorWorkspaceRootWindowState
	{
		checkf(!bWindowBegun, "An editor workspace root window must be ended before it begins again");
		EditorWorkspaceUI::SetNextEditorRootWindowClass();
		if (Config.bDockInEditorHost)
			ImGui::SetNextWindowDockID(EditorWorkspaceUI::MakeEditorHostDockSpaceId(EditorWorkspaceUI::HostLayoutVersion), ImGuiCond_FirstUseEver);
		if (bFocusRequested)
		{
			ImGui::SetNextWindowFocus();
			bFocusRequested = false;
		}

		const std::string DisplayName = Config.bDirty ? std::format("{} *", Config.DisplayName) : std::string(Config.DisplayName);
		const std::string RootWindowName = EditorWorkspaceUI::MakeEditorRootWindowName(DisplayName, Config.RootKey);
		if (Config.bZeroPadding) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		bool bOpen = true;
		const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoCollapse | Config.AdditionalFlags |
			(Config.bDirty ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None);
		FEditorWorkspaceRootWindowState State;
		State.bVisible = ImGui::Begin(RootWindowName.c_str(), &bOpen, Flags);
		bWindowBegun = true;
		if (Config.bZeroPadding) ImGui::PopStyleVar();
		State.bFocused = State.bVisible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		const bool bDockTabSelected = State.bVisible && ImGui::IsWindowDocked();
		// A selected dock tab may not transfer focus to one of its child controls.
		State.bActivated = bDockTabSelected && !bWasDockTabSelected;
		bWasDockTabSelected = bDockTabSelected;
		State.bCloseRequested = !bOpen;

		if (!State.bVisible && Config.InternalDockSpace &&
			ImGui::DockBuilderGetNode(EditorWorkspaceUI::MakeDockSpaceId(
				Config.InternalDockSpace->WorkspaceType,
				Config.InternalDockSpace->LayoutVersion
			)) != nullptr)
		{
			EditorWorkspaceUI::SubmitDockSpace(
				Config.InternalDockSpace->WorkspaceType,
				Config.InternalDockSpace->LayoutVersion,
				ImVec2(0.0f, 0.0f),
				ImGuiDockNodeFlags_KeepAliveOnly
			);
		}
		return State;
	}

	auto FEditorWorkspaceRootWindow::End() -> void
	{
		checkf(bWindowBegun, "An editor workspace root window must begin before it ends");
		ImGui::End();
		bWindowBegun = false;
	}
}
