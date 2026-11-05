#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"
#include "Documents/EditorDocumentTypes.h"
#include "MonaImGui.h"

namespace Durin::Editor::TabLayout
{
	inline constexpr uint32 MainFrameLayoutVersion = 3;

	DURINED_API auto MakeMainFrameDockSpaceName(uint32 LayoutVersion) -> std::string;
	DURINED_API auto MakeRootWindowName(std::string_view DisplayName, std::string_view RootKey) -> std::string;
	DURINED_API auto MakeDocumentRootKey(std::string_view WorkspaceRootKey, std::string_view DocumentKey) -> std::string;
	DURINED_API auto MakeDockClassName(const FAssetEditorToolkitId& ToolkitId) -> std::string;
	DURINED_API auto MakeDockSpaceName(const FAssetEditorToolkitId& ToolkitId, uint32 LayoutVersion) -> std::string;
	DURINED_API auto MakePanelWindowName(std::string_view DisplayName, const FAssetEditorToolkitId& ToolkitId, std::string_view PanelKey) -> std::string;
	DURINED_API auto MakeRootDockClassId() -> ImGuiID;
	DURINED_API auto MakeMainFrameDockSpaceId(uint32 LayoutVersion) -> ImGuiID;
	DURINED_API auto MakeDockClassId(const FAssetEditorToolkitId& ToolkitId) -> ImGuiID;
	DURINED_API auto MakeDockSpaceId(const FAssetEditorToolkitId& ToolkitId, uint32 LayoutVersion) -> ImGuiID;
	DURINED_API auto MakeRootWindowClass() -> ImGuiWindowClass;
	DURINED_API auto MakeWindowClass(const FAssetEditorToolkitId& ToolkitId) -> ImGuiWindowClass;
	DURINED_API auto SetNextRootWindowClass() -> void;
	DURINED_API auto SetNextDockableWindowClass(const FAssetEditorToolkitId& ToolkitId) -> void;
	DURINED_API auto BeginDockablePanel(
		const FAssetEditorToolkitId& ToolkitId,
		std::string_view DisplayName,
		std::string_view PanelKey,
		bool* bOpen = nullptr,
		ImGuiWindowFlags Flags = ImGuiWindowFlags_None
	) -> bool;
	DURINED_API auto SubmitDockSpace(
		const FAssetEditorToolkitId& ToolkitId,
		uint32 LayoutVersion,
		const ImVec2& Size,
		ImGuiDockNodeFlags Flags = ImGuiDockNodeFlags_None
	) -> ImGuiID;
	DURINED_API auto SubmitMainFrameDockSpace(
		uint32 LayoutVersion,
		const ImVec2& Size,
		ImGuiDockNodeFlags Flags = ImGuiDockNodeFlags_None
	) -> ImGuiID;
}
