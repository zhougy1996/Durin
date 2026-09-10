#pragma once

#include "Editor/WorkspaceTypes.h"
#include "Editor/WorkspaceUI.h"
#include "MonaImGuiWidgets.h"

namespace Durin::Editor::Material::Workspace
{
	inline const ::Durin::Editor::FWorkspaceTypeId Type("MaterialEditor");
	inline constexpr std::string_view RootKey = "MaterialEditor";
	inline constexpr uint32 LayoutVersion = 1;

	// Isolates panel docking and persisted layouts for each material document.
	inline auto MakeDocumentDockType(const ::Durin::Editor::FDocumentTab& Document)
		-> ::Durin::Editor::FWorkspaceTypeId
	{
		return ::Durin::Editor::FWorkspaceTypeId(std::format("MaterialEditor.{}", Document.DocumentKey));
	}

	inline auto BuildDefaultLayout(
		const ::Durin::Editor::FDocumentTab& Document, const ImVec2& Size) -> void
	{
		const auto DockType = MakeDocumentDockType(Document);
		const ImGuiID DockSpaceId = ::Durin::Editor::WorkspaceUI::MakeDockSpaceId(DockType, LayoutVersion);
		ImGui::DockBuilderRemoveNode(DockSpaceId);
		ImGui::DockBuilderAddNode(DockSpaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(DockSpaceId, Size);
		ImGui::DockBuilderGetNode(DockSpaceId)->WindowClass = ::Durin::Editor::WorkspaceUI::MakeWindowClass(DockType);

		ImGuiID GraphId = DockSpaceId;
		ImGuiID PreviewId = GraphId;
		ImGuiID DetailsId = GraphId;
		ImGuiID DiagnosticsId = GraphId;
		if (Size.x >= MonaImGui::ScaleUI(980.0f))
		{
			DetailsId = ImGui::DockBuilderSplitNode(GraphId, ImGuiDir_Right, 0.26f, nullptr, &GraphId);
			PreviewId = ImGui::DockBuilderSplitNode(GraphId, ImGuiDir_Left, 0.28f, nullptr, &GraphId);
			DiagnosticsId = ImGui::DockBuilderSplitNode(GraphId, ImGuiDir_Down, 0.25f, nullptr, &GraphId);
		}
		// Small initial windows use real dock tabs; resizing never overwrites the user's arrangement.
		const auto DockPanel = [&](const char* Label, const char* Key, ImGuiID NodeId) {
			const auto Name = ::Durin::Editor::WorkspaceUI::MakePanelWindowName(Label, DockType, Key);
			ImGui::DockBuilderDockWindow(Name.c_str(), NodeId);
		};
		DockPanel("Material Graph", "Graph", GraphId);
		DockPanel("Preview", "Preview", PreviewId);
		DockPanel("Details", "Details", DetailsId);
		DockPanel("Diagnostics", "Diagnostics", DiagnosticsId);
		ImGui::DockBuilderFinish(DockSpaceId);
	}

}
