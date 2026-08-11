#pragma once

#include "DurinEdAPI.h"
#include "Editor/WorkspaceTypes.h"
#include "MonaImGui.h"

namespace Durin::Editor
{
	class FWorkspaceManager;
}
namespace Durin::Editor::WorkspaceUI
{
	inline constexpr uint32 HostLayoutVersion = 2;

	DURINED_API auto MakeHostDockSpaceName(uint32 LayoutVersion) -> std::string;
	DURINED_API auto MakeRootWindowName(std::string_view DisplayName, std::string_view RootKey) -> std::string;
	DURINED_API auto MakeDocumentRootKey(std::string_view WorkspaceRootKey, std::string_view DocumentKey) -> std::string;
	DURINED_API auto MakeDockClassName(const FWorkspaceTypeId& WorkspaceType) -> std::string;
	DURINED_API auto MakeDockSpaceName(const FWorkspaceTypeId& WorkspaceType, uint32 LayoutVersion) -> std::string;
	DURINED_API auto MakePanelWindowName(std::string_view DisplayName, const FWorkspaceTypeId& WorkspaceType, std::string_view PanelKey) -> std::string;
	DURINED_API auto MakeRootDockClassId() -> ImGuiID;
	DURINED_API auto MakeHostDockSpaceId(uint32 LayoutVersion) -> ImGuiID;
	DURINED_API auto MakeDockClassId(const FWorkspaceTypeId& WorkspaceType) -> ImGuiID;
	DURINED_API auto MakeDockSpaceId(const FWorkspaceTypeId& WorkspaceType, uint32 LayoutVersion) -> ImGuiID;
	DURINED_API auto MakeRootWindowClass() -> ImGuiWindowClass;
	DURINED_API auto MakeWindowClass(const FWorkspaceTypeId& WorkspaceType) -> ImGuiWindowClass;
	DURINED_API auto SetNextRootWindowClass() -> void;
	DURINED_API auto SetNextDockableWindowClass(const FWorkspaceTypeId& WorkspaceType) -> void;
	DURINED_API auto BeginDockablePanel(
		const FWorkspaceTypeId& WorkspaceType,
		std::string_view DisplayName,
		std::string_view PanelKey,
		bool* bOpen = nullptr,
		ImGuiWindowFlags Flags = ImGuiWindowFlags_None
	) -> bool;
	DURINED_API auto SubmitDockSpace(
		const FWorkspaceTypeId& WorkspaceType,
		uint32 LayoutVersion,
		const ImVec2& Size,
		ImGuiDockNodeFlags Flags = ImGuiDockNodeFlags_None
	) -> ImGuiID;
	DURINED_API auto SubmitHostDockSpace(
		uint32 LayoutVersion,
		const ImVec2& Size,
		ImGuiDockNodeFlags Flags = ImGuiDockNodeFlags_None
	) -> ImGuiID;
	DURINED_API auto DrawDocumentCloseConfirmation(FWorkspaceManager& WorkspaceManager) -> void;
}
