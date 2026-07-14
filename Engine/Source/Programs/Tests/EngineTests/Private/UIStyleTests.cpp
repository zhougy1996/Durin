#include <gtest/gtest.h>

#include "LevelEditorUILayout.h"
#include "MonaImGui.h"

namespace Durin
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
} // namespace Durin
