#include "Editor/WorkspaceUI.h"

#include "Editor/WorkspaceManager.h"

namespace Durin::Editor::WorkspaceUI
{
	auto MakeHostDockSpaceName(uint32 LayoutVersion) -> std::string
	{
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

	auto MakeDockClassName(const FWorkspaceTypeId& WorkspaceType) -> std::string
	{
		return std::format("Durin.DockClass.{}", WorkspaceType.GetValue());
	}

	auto MakeDockSpaceName(const FWorkspaceTypeId& WorkspaceType, uint32 LayoutVersion) -> std::string
	{
		return std::format("Durin.DockSpace.{}.v{}", WorkspaceType.GetValue(), LayoutVersion);
	}

	auto MakePanelWindowName(std::string_view DisplayName, const FWorkspaceTypeId& WorkspaceType, std::string_view PanelKey) -> std::string
	{
		return std::format("{}###Durin.{}.Panel.{}", DisplayName, WorkspaceType.GetValue(), PanelKey);
	}

	auto MakeRootDockClassId() -> ImGuiID
	{
		return ImHashStr("Durin.DockClass.EditorRoot");
	}

	auto MakeHostDockSpaceId(uint32 LayoutVersion) -> ImGuiID
	{
		const std::string Name = MakeHostDockSpaceName(LayoutVersion);
		return ImHashStr(Name.c_str());
	}

	auto MakeDockClassId(const FWorkspaceTypeId& WorkspaceType) -> ImGuiID
	{
		const std::string Name = MakeDockClassName(WorkspaceType);
		return ImHashStr(Name.c_str());
	}

	auto MakeDockSpaceId(const FWorkspaceTypeId& WorkspaceType, uint32 LayoutVersion) -> ImGuiID
	{
		const std::string Name = MakeDockSpaceName(WorkspaceType, LayoutVersion);
		return ImHashStr(Name.c_str());
	}

	auto MakeRootWindowClass() -> ImGuiWindowClass
	{
		ImGuiWindowClass WindowClass;
		WindowClass.ClassId = MakeRootDockClassId();
		WindowClass.DockingAllowUnclassed = false;
		return WindowClass;
	}

	auto MakeWindowClass(const FWorkspaceTypeId& WorkspaceType) -> ImGuiWindowClass
	{
		ImGuiWindowClass WindowClass;
		WindowClass.ClassId = MakeDockClassId(WorkspaceType);
		WindowClass.DockingAllowUnclassed = false;
		return WindowClass;
	}

	auto SetNextRootWindowClass() -> void
	{
		const ImGuiWindowClass WindowClass = MakeRootWindowClass();
		ImGui::SetNextWindowClass(&WindowClass);
	}

	auto SetNextDockableWindowClass(const FWorkspaceTypeId& WorkspaceType) -> void
	{
		const ImGuiWindowClass WindowClass = MakeWindowClass(WorkspaceType);
		ImGui::SetNextWindowClass(&WindowClass);
	}

	auto BeginDockablePanel(
		const FWorkspaceTypeId& WorkspaceType,
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
		const FWorkspaceTypeId& WorkspaceType,
		uint32 LayoutVersion,
		const ImVec2& Size,
		ImGuiDockNodeFlags Flags
	) -> ImGuiID
	{
		const ImGuiWindowClass WindowClass = MakeWindowClass(WorkspaceType);
		const ImGuiID DockSpaceId = MakeDockSpaceId(WorkspaceType, LayoutVersion);
		return ImGui::DockSpace(DockSpaceId, Size, Flags, &WindowClass);
	}

	auto SubmitHostDockSpace(uint32 LayoutVersion, const ImVec2& Size, ImGuiDockNodeFlags Flags) -> ImGuiID
	{
		const ImGuiWindowClass WindowClass = MakeRootWindowClass();
		const ImGuiID DockSpaceId = MakeHostDockSpaceId(LayoutVersion);
		return ImGui::DockSpace(DockSpaceId, Size, Flags, &WindowClass);
	}

	auto DrawDocumentCloseConfirmation(FWorkspaceManager& WorkspaceManager) -> void
	{
		const FDocumentTab* PendingDocument = WorkspaceManager.GetPendingCloseDocument();
		if (!PendingDocument) return;

		ImGui::OpenPopup("Confirm Close Document###Durin.Editor.DocumentCloseConfirmation");
		if (!ImGui::BeginPopupModal(
				"Confirm Close Document###Durin.Editor.DocumentCloseConfirmation",
				nullptr,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings
			))
			return;

		ImGui::TextWrapped("Save changes to \"%s\" before closing?", PendingDocument->Label.c_str());
		ImGui::Spacing();
		if (ImGui::Button("Save", ImVec2(100, 0)))
		{
			if (WorkspaceManager.ResolvePendingDocumentClose(EDocumentCloseResponse::Save) ==
				EDocumentCloseResult::Closed)
				ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Discard", ImVec2(100, 0)))
		{
			if (WorkspaceManager.ResolvePendingDocumentClose(EDocumentCloseResponse::Discard) ==
				EDocumentCloseResult::Closed)
				ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100, 0)))
		{
			WorkspaceManager.ResolvePendingDocumentClose(EDocumentCloseResponse::Cancel);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}
