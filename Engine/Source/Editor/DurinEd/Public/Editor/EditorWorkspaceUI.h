#pragma once

#include "DurinEdAPI.h"
#include "Editor/EditorWorkspace.h"
#include "MonaImGui.h"

namespace Durin::EditorWorkspaceUI
{
	DURINED_API auto MakeEditorHostDockSpaceName(uint32 LayoutVersion) -> std::string;
	DURINED_API auto MakeEditorRootWindowName(std::string_view DisplayName, std::string_view RootKey) -> std::string;
	DURINED_API auto MakeDockClassName(const FEditorWorkspaceTypeId& WorkspaceType) -> std::string;
	DURINED_API auto MakeDockSpaceName(const FEditorWorkspaceTypeId& WorkspaceType, uint32 LayoutVersion) -> std::string;
	DURINED_API auto MakePanelWindowName(std::string_view DisplayName, const FEditorWorkspaceTypeId& WorkspaceType, std::string_view PanelKey) -> std::string;
	DURINED_API auto MakeEditorRootDockClassId() -> ImGuiID;
	DURINED_API auto MakeEditorHostDockSpaceId(uint32 LayoutVersion) -> ImGuiID;
	DURINED_API auto MakeDockClassId(const FEditorWorkspaceTypeId& WorkspaceType) -> ImGuiID;
	DURINED_API auto MakeDockSpaceId(const FEditorWorkspaceTypeId& WorkspaceType, uint32 LayoutVersion) -> ImGuiID;
	DURINED_API auto MakeEditorRootWindowClass() -> ImGuiWindowClass;
	DURINED_API auto MakeWindowClass(const FEditorWorkspaceTypeId& WorkspaceType) -> ImGuiWindowClass;
	DURINED_API auto SetNextEditorRootWindowClass() -> void;
	DURINED_API auto SetNextDockableWindowClass(const FEditorWorkspaceTypeId& WorkspaceType) -> void;
	DURINED_API auto BeginDockablePanel(
		const FEditorWorkspaceTypeId& WorkspaceType,
		std::string_view DisplayName,
		std::string_view PanelKey,
		bool* bOpen = nullptr,
		ImGuiWindowFlags Flags = ImGuiWindowFlags_None
	) -> bool;
	DURINED_API auto SubmitDockSpace(
		const FEditorWorkspaceTypeId& WorkspaceType,
		uint32 LayoutVersion,
		const ImVec2& Size,
		ImGuiDockNodeFlags Flags = ImGuiDockNodeFlags_None
	) -> ImGuiID;
	DURINED_API auto SubmitEditorHostDockSpace(
		uint32 LayoutVersion,
		const ImVec2& Size,
		ImGuiDockNodeFlags Flags = ImGuiDockNodeFlags_None
	) -> ImGuiID;
}
