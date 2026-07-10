#include "Widgets/MLevelEditor.h"

#include "Engine/Engine.h"
#include "LevelEditorContext.h"
#include "MonaImGui.h"
#include "Panels/DetailsPanel.h"
#include "Panels/LevelEditorPanel.h"
#include "Panels/OutputLogPanel.h"
#include "Panels/SceneViewportPanel.h"
#include "Panels/WorldOutlinerPanel.h"

namespace Durin
{
	namespace
	{
		constexpr const char* DockSpaceName = "DurinEditorDockSpace";
	}

	MLevelEditor::MLevelEditor() = default;
	MLevelEditor::~MLevelEditor() = default;

	auto MLevelEditor::Construct() -> void
	{
		Context = std::make_unique<FLevelEditorContext>();
		Panels.emplace_back(std::make_unique<FSceneViewportPanel>());
		Panels.emplace_back(std::make_unique<FWorldOutlinerPanel>());
		Panels.emplace_back(std::make_unique<FDetailsPanel>());
		Panels.emplace_back(std::make_unique<FOutputLogPanel>());
	}

	auto MLevelEditor::Draw() -> void
	{
		if (!Context)
		{
			return;
		}

		Context->Synchronize(GEngine != nullptr ? GEngine->GetWorld() : nullptr);
		DrawMainMenu();

		ImGuiViewport* MainViewport = ImGui::GetMainViewport();
		const ImGuiID DockSpaceId = ImGui::GetID(DockSpaceName);
		const bool bNeedsDefaultLayout = ImGui::DockBuilderGetNode(DockSpaceId) == nullptr;
		ImGui::DockSpaceOverViewport(DockSpaceId, MainViewport, ImGuiDockNodeFlags_None);
		if (bNeedsDefaultLayout || bResetLayoutRequested)
		{
			BuildDefaultLayout(DockSpaceId);
			bResetLayoutRequested = false;
		}

		for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
		{
			if (Panel->IsOpen())
			{
				Panel->Draw(*Context);
			}
		}
	}

	auto MLevelEditor::DrawMainMenu() -> void
	{
		if (!ImGui::BeginMainMenuBar())
		{
			return;
		}

		if (ImGui::BeginMenu("File"))
		{
			ImGui::MenuItem("No file actions available", nullptr, false, false);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit"))
		{
			ImGui::MenuItem("Undo/redo is not available yet", nullptr, false, false);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::MenuItem("Reset Layout"))
			{
				bResetLayoutRequested = true;
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Window"))
		{
			for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
			{
				bool bOpen = Panel->IsOpen();
				if (ImGui::MenuItem(Panel->GetWindowName(), nullptr, &bOpen))
				{
					Panel->SetOpen(bOpen);
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Help"))
		{
			ImGui::MenuItem("Durin Level Editor - early development", nullptr, false, false);
			ImGui::EndMenu();
		}

		ImGui::EndMainMenuBar();
	}

	auto MLevelEditor::BuildDefaultLayout(uint32 DockSpaceId) -> void
	{
		ImGuiViewport* MainViewport = ImGui::GetMainViewport();
		ImGui::DockBuilderRemoveNode(DockSpaceId);
		ImGui::DockBuilderAddNode(DockSpaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(DockSpaceId, MainViewport->WorkSize);

		ImGuiID MainDockId = DockSpaceId;
		const ImGuiID LeftDockId = ImGui::DockBuilderSplitNode(MainDockId, ImGuiDir_Left, 0.20f, nullptr, &MainDockId);
		const ImGuiID RightDockId = ImGui::DockBuilderSplitNode(MainDockId, ImGuiDir_Right, 0.25f, nullptr, &MainDockId);
		const ImGuiID BottomDockId = ImGui::DockBuilderSplitNode(MainDockId, ImGuiDir_Down, 0.25f, nullptr, &MainDockId);

		ImGui::DockBuilderDockWindow("World Outliner###WorldOutliner", LeftDockId);
		ImGui::DockBuilderDockWindow("Details###Details", RightDockId);
		ImGui::DockBuilderDockWindow("Output Log###OutputLog", BottomDockId);
		ImGui::DockBuilderDockWindow("Scene Viewport###SceneViewport", MainDockId);
		ImGui::DockBuilderFinish(DockSpaceId);
	}
} // namespace Durin
