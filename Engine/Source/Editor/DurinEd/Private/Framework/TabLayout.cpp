#include "Framework/TabLayout.h"
#include "ThirdParty/ImGui/imgui_internal.h"


namespace Durin::Editor::TabLayout
{
	auto MakeMainFrameDockSpaceName(uint32 LayoutVersion) -> std::string
	{
		// Persisted docking identity predates the MainFrame API naming.
		return std::format("Durin.DockSpace.EditorHost.v{}", LayoutVersion);
	}

	auto MakeRootWindowName(std::string_view DisplayName, std::string_view RootKey) -> std::string
	{
		return std::format("{}###Durin.Editor.Root.{}", DisplayName, RootKey);
	}

	auto MakeDocumentRootKey(std::string_view WorkspaceRootKey, std::string_view DocumentKey) -> std::string
	{
		return std::format("{}.{}", WorkspaceRootKey, DocumentKey);
	}

	auto MakeDockClassName(const FAssetEditorToolkitId& ToolkitId) -> std::string
	{
		return std::format("Durin.DockClass.{}", ToolkitId.GetValue());
	}

	auto MakeDockSpaceName(const FAssetEditorToolkitId& ToolkitId, uint32 LayoutVersion) -> std::string
	{
		return std::format("Durin.DockSpace.{}.v{}", ToolkitId.GetValue(), LayoutVersion);
	}

	auto MakePanelWindowName(std::string_view DisplayName, const FAssetEditorToolkitId& ToolkitId, std::string_view PanelKey) -> std::string
	{
		return std::format("{}###Durin.{}.Panel.{}", DisplayName, ToolkitId.GetValue(), PanelKey);
	}

	auto MakeRootDockClassId() -> ImGuiID
	{
		return ImHashStr("Durin.DockClass.EditorRoot");
	}

	auto MakeMainFrameDockSpaceId(uint32 LayoutVersion) -> ImGuiID
	{
		const std::string Name = MakeMainFrameDockSpaceName(LayoutVersion);
		return ImHashStr(Name.c_str());
	}

	auto MakeDockClassId(const FAssetEditorToolkitId& ToolkitId) -> ImGuiID
	{
		const std::string Name = MakeDockClassName(ToolkitId);
		return ImHashStr(Name.c_str());
	}

	auto MakeDockSpaceId(const FAssetEditorToolkitId& ToolkitId, uint32 LayoutVersion) -> ImGuiID
	{
		const std::string Name = MakeDockSpaceName(ToolkitId, LayoutVersion);
		return ImHashStr(Name.c_str());
	}

	auto MakeRootWindowClass() -> ImGuiWindowClass
	{
		ImGuiWindowClass WindowClass;
		WindowClass.ClassId = MakeRootDockClassId();
		WindowClass.DockingAllowUnclassed = false;
		return WindowClass;
	}

	auto MakeWindowClass(const FAssetEditorToolkitId& ToolkitId) -> ImGuiWindowClass
	{
		ImGuiWindowClass WindowClass;
		WindowClass.ClassId = MakeDockClassId(ToolkitId);
		WindowClass.DockingAllowUnclassed = false;
		return WindowClass;
	}

	auto SetNextRootWindowClass() -> void
	{
		const ImGuiWindowClass WindowClass = MakeRootWindowClass();
		ImGui::SetNextWindowClass(&WindowClass);
	}

	auto SetNextDockableWindowClass(const FAssetEditorToolkitId& ToolkitId) -> void
	{
		const ImGuiWindowClass WindowClass = MakeWindowClass(ToolkitId);
		ImGui::SetNextWindowClass(&WindowClass);
	}

	auto BeginDockablePanel(
		const FAssetEditorToolkitId& ToolkitId,
		std::string_view DisplayName,
		std::string_view PanelKey,
		bool* bOpen,
		ImGuiWindowFlags Flags
	) -> bool
	{
		SetNextDockableWindowClass(ToolkitId);
		const std::string WindowName = MakePanelWindowName(DisplayName, ToolkitId, PanelKey);
		return ImGui::Begin(WindowName.c_str(), bOpen, Flags);
	}

	auto SubmitDockSpace(
		const FAssetEditorToolkitId& ToolkitId,
		uint32 LayoutVersion,
		const ImVec2& Size,
		ImGuiDockNodeFlags Flags
	) -> ImGuiID
	{
		const ImGuiWindowClass WindowClass = MakeWindowClass(ToolkitId);
		const ImGuiID DockSpaceId = MakeDockSpaceId(ToolkitId, LayoutVersion);
		return ImGui::DockSpace(DockSpaceId, Size, Flags, &WindowClass);
	}

	auto SubmitMainFrameDockSpace(uint32 LayoutVersion, const ImVec2& Size, ImGuiDockNodeFlags Flags) -> ImGuiID
	{
		const ImGuiWindowClass WindowClass = MakeRootWindowClass();
		const ImGuiID DockSpaceId = MakeMainFrameDockSpaceId(LayoutVersion);
		return ImGui::DockSpace(DockSpaceId, Size, Flags, &WindowClass);
	}

}
