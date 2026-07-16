#include "Editor/EditorWorkspaceUI.h"

namespace Durin::EditorWorkspaceUI
{
	auto MakeEditorHostDockSpaceName(uint32 LayoutVersion) -> std::string
	{
		return std::format("Durin.DockSpace.EditorHost.v{}", LayoutVersion);
	}

	auto MakeEditorRootWindowName(std::string_view DisplayName, std::string_view RootKey) -> std::string
	{
		return std::format("{}###Durin.Editor.Root.{}", DisplayName, RootKey);
	}

	auto MakeDockClassName(const FEditorWorkspaceTypeId& WorkspaceType) -> std::string
	{
		return std::format("Durin.DockClass.{}", WorkspaceType.GetValue());
	}

	auto MakeDockSpaceName(const FEditorWorkspaceTypeId& WorkspaceType, uint32 LayoutVersion) -> std::string
	{
		return std::format("Durin.DockSpace.{}.v{}", WorkspaceType.GetValue(), LayoutVersion);
	}

	auto MakePanelWindowName(std::string_view DisplayName, const FEditorWorkspaceTypeId& WorkspaceType, std::string_view PanelKey) -> std::string
	{
		return std::format("{}###Durin.{}.Panel.{}", DisplayName, WorkspaceType.GetValue(), PanelKey);
	}

	auto MakeEditorRootDockClassId() -> ImGuiID
	{
		return ImHashStr("Durin.DockClass.EditorRoot");
	}

	auto MakeEditorHostDockSpaceId(uint32 LayoutVersion) -> ImGuiID
	{
		const std::string Name = MakeEditorHostDockSpaceName(LayoutVersion);
		return ImHashStr(Name.c_str());
	}

	auto MakeDockClassId(const FEditorWorkspaceTypeId& WorkspaceType) -> ImGuiID
	{
		const std::string Name = MakeDockClassName(WorkspaceType);
		return ImHashStr(Name.c_str());
	}

	auto MakeDockSpaceId(const FEditorWorkspaceTypeId& WorkspaceType, uint32 LayoutVersion) -> ImGuiID
	{
		const std::string Name = MakeDockSpaceName(WorkspaceType, LayoutVersion);
		return ImHashStr(Name.c_str());
	}

	auto MakeEditorRootWindowClass() -> ImGuiWindowClass
	{
		ImGuiWindowClass WindowClass;
		WindowClass.ClassId = MakeEditorRootDockClassId();
		WindowClass.DockingAllowUnclassed = false;
		return WindowClass;
	}

	auto MakeWindowClass(const FEditorWorkspaceTypeId& WorkspaceType) -> ImGuiWindowClass
	{
		ImGuiWindowClass WindowClass;
		WindowClass.ClassId = MakeDockClassId(WorkspaceType);
		WindowClass.DockingAllowUnclassed = false;
		return WindowClass;
	}

	auto SetNextEditorRootWindowClass() -> void
	{
		const ImGuiWindowClass WindowClass = MakeEditorRootWindowClass();
		ImGui::SetNextWindowClass(&WindowClass);
	}

	auto SetNextDockableWindowClass(const FEditorWorkspaceTypeId& WorkspaceType) -> void
	{
		const ImGuiWindowClass WindowClass = MakeWindowClass(WorkspaceType);
		ImGui::SetNextWindowClass(&WindowClass);
	}

	auto BeginDockablePanel(
		const FEditorWorkspaceTypeId& WorkspaceType,
		std::string_view DisplayName,
		std::string_view PanelKey,
		bool* bOpen,
		ImGuiWindowFlags Flags
	) -> bool
	{
		SetNextDockableWindowClass(WorkspaceType);
		const std::string WindowName = MakePanelWindowName(DisplayName, WorkspaceType, PanelKey);
		return ImGui::Begin(WindowName.c_str(), bOpen, Flags);
	}

	auto SubmitDockSpace(
		const FEditorWorkspaceTypeId& WorkspaceType,
		uint32 LayoutVersion,
		const ImVec2& Size,
		ImGuiDockNodeFlags Flags
	) -> ImGuiID
	{
		const ImGuiWindowClass WindowClass = MakeWindowClass(WorkspaceType);
		const ImGuiID DockSpaceId = MakeDockSpaceId(WorkspaceType, LayoutVersion);
		return ImGui::DockSpace(DockSpaceId, Size, Flags, &WindowClass);
	}

	auto SubmitEditorHostDockSpace(uint32 LayoutVersion, const ImVec2& Size, ImGuiDockNodeFlags Flags) -> ImGuiID
	{
		const ImGuiWindowClass WindowClass = MakeEditorRootWindowClass();
		const ImGuiID DockSpaceId = MakeEditorHostDockSpaceId(LayoutVersion);
		return ImGui::DockSpace(DockSpaceId, Size, Flags, &WindowClass);
	}
}
