#include <gtest/gtest.h>

#include "Editor/WorkspaceUI.h"
#include "MonaImGui.h"
#include "Panels/ConsolePanelLayout.h"

namespace Durin::Editor::MainFrame
{
	TEST(FEditorHostToolTests, ClipsVariableHeightConsoleRecordsWithOverscan)
	{
		const std::array<float, 5> Offsets{0.0f, 20.0f, 60.0f, 80.0f, 140.0f};
		const FConsoleVisibleRange Middle =
			ResolveConsoleVisibleRange(Offsets, 55.0f, 30.0f);
		EXPECT_EQ(Middle.Begin, 0u);
		EXPECT_EQ(Middle.End, 4u);

		const FConsoleVisibleRange Bottom =
			ResolveConsoleVisibleRange(Offsets, 120.0f, 30.0f);
		EXPECT_EQ(Bottom.Begin, 2u);
		EXPECT_EQ(Bottom.End, 4u);
		EXPECT_EQ(ResolveConsoleVisibleRange({}, 0.0f, 100.0f).End, 0u);
	}

	TEST(FEditorHostToolTests, PlacesStatusBarDirectlyBelowHostDockSpace)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);
		ImGuiIO& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		IO.DisplaySize = ImVec2(1280.0f, 720.0f);
		IO.DeltaTime = 1.0f / 60.0f;
		IO.Fonts->Build();

		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(IO.DisplaySize);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Host", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		ImGui::PopStyleVar();

		const ImVec2 DockMin = ImGui::GetCursorScreenPos();
		const ImVec2 DockSize(1280.0f, 690.0f);
		const ImVec2 HostItemSpacing(ImGui::GetStyle().ItemSpacing.x, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, HostItemSpacing);
		WorkspaceUI::SubmitHostDockSpace(
			WorkspaceUI::HostLayoutVersion, DockSize);
		ImGui::PopStyleVar();

		ASSERT_TRUE(ImGui::BeginChild("StatusBar", ImVec2(0.0f, 30.0f)));
		EXPECT_FLOAT_EQ(ImGui::GetWindowPos().y, DockMin.y + DockSize.y);
		EXPECT_FLOAT_EQ(ImGui::GetWindowSize().y, 30.0f);
		ImGui::EndChild();
		ImGui::End();
		ImGui::Render();
		ImGui::DestroyContext(Context);
	}
} // namespace Durin::Editor::MainFrame
