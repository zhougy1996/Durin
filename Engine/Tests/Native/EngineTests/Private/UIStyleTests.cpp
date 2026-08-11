#include <gtest/gtest.h>

#include "Workspace/LevelEditorUILayout.h"
#include "MonaImGui.h"

namespace Durin::Editor::Level
{
	TEST(FEditorUILayoutTests, SelectsResponsiveModeAtBoundaries)
	{
		EXPECT_EQ(ResolveEditorUILayout(-1.0f, 520.0f, 780.0f), EEditorUILayoutMode::Narrow);
		EXPECT_EQ(ResolveEditorUILayout(519.0f, 520.0f, 780.0f), EEditorUILayoutMode::Narrow);
		EXPECT_EQ(ResolveEditorUILayout(520.0f, 520.0f, 780.0f), EEditorUILayoutMode::Compact);
		EXPECT_EQ(ResolveEditorUILayout(779.0f, 520.0f, 780.0f), EEditorUILayoutMode::Compact);
		EXPECT_EQ(ResolveEditorUILayout(780.0f, 520.0f, 780.0f), EEditorUILayoutMode::Full);
	}

	TEST(FMonaImGuiStyleTests, ClampsScaleAndReturnsScaledMetrics)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);

		MonaImGui::SetGlobalUIScale(0.25f);
		EXPECT_FLOAT_EQ(MonaImGui::GetGlobalUIScale(), 0.75f);
		EXPECT_FLOAT_EQ(MonaImGui::ScaleUI(8.0f), 6.0f);
		EXPECT_FLOAT_EQ(MonaImGui::GetUIStyleMetrics().SplitterThickness, 4.5f);

		MonaImGui::SetGlobalUIScale(4.0f);
		EXPECT_FLOAT_EQ(MonaImGui::GetGlobalUIScale(), 2.0f);
		EXPECT_FLOAT_EQ(MonaImGui::GetUIStyleMetrics().StandardButtonWidth, 192.0f);

		MonaImGui::SetGlobalUIScale(1.0f);
		ImGui::DestroyContext(Context);
	}

	TEST(FMonaImGuiStyleTests, ThemeProvidesSemanticFallbackColors)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);
		MonaImGui::SetColorTheme(MonaImGui::EColorTheme::Dark);
		const ImVec4 DarkWarning = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning);
		EXPECT_GT(DarkWarning.w, 0.0f);
		MonaImGui::SetColorTheme(MonaImGui::EColorTheme::Light);
		const ImVec4 LightWarning = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning);
		EXPECT_GT(LightWarning.w, 0.0f);
		EXPECT_NE(DarkWarning.x, LightWarning.x);
		MonaImGui::SetColorTheme(MonaImGui::EColorTheme::Dark);
		ImGui::DestroyContext(Context);
	}

	TEST(FMonaImGuiStyleTests, VectorDragDirectInputRejectsNonNumericCharacters)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);
		ImGuiIO& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(800.0f, 600.0f);
		IO.DeltaTime = 1.0f / 60.0f;
		IO.Fonts->Build();
		FVector3 StoredValue(0.0, 2.126, 0.870);

		auto DrawFrame = [&]() {
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(600.0f, 300.0f));
			ImGui::Begin("VectorInputTest", nullptr, ImGuiWindowFlags_NoTitleBar);
			FVector3 EditedValue = StoredValue;
			if (MonaImGui::PropertyEdit::EditVectorValue("Value", EditedValue))
				StoredValue = EditedValue;
			ImGui::End();
			ImGui::Render();
		};

		DrawFrame();
		// The third equal-width component occupies the right side of the fixed test window.
		IO.AddMousePosEvent(500.0f, 16.0f);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		DrawFrame();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		DrawFrame();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		DrawFrame();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		DrawFrame();
		IO.AddInputCharactersUTF8("abc42");
		DrawFrame();

		EXPECT_DOUBLE_EQ(StoredValue.z, 42.0);
		ImGui::DestroyContext(Context);
	}
} // namespace Durin::Editor::Level
