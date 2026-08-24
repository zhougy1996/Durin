#include <gtest/gtest.h>

#include "Panels/ConsolePanelLayout.h"
#include "Workspace/LevelEditorUILayout.h"
#include "Workspace/LevelEditorPresentationPolicy.h"
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

	TEST(FEditorUILayoutTests, AppliesViewportFirstPanelAndUnreadPolicies)
	{
		EXPECT_TRUE(IsLevelEditorPanelOpenByDefault(
			ELevelEditorPanelRole::Persistent));
		EXPECT_FALSE(IsLevelEditorPanelOpenByDefault(
			ELevelEditorPanelRole::Optional));
		EXPECT_FALSE(IsLevelEditorPanelOpenByDefault(
			ELevelEditorPanelRole::DrawerTool));
		EXPECT_FALSE(IsLevelEditorPanelOpenByDefault(
			ELevelEditorPanelRole::ActivityHistory));
		EXPECT_EQ(ResolveDrawerToggleDisposition(false, false, false),
			EDrawerToggleDisposition::OpenDrawer);
		EXPECT_EQ(ResolveDrawerToggleDisposition(false, true, true),
			EDrawerToggleDisposition::CloseDrawer);
		EXPECT_EQ(ResolveDrawerToggleDisposition(false, true, false),
			EDrawerToggleDisposition::OpenDrawer);
		EXPECT_EQ(ResolveDrawerToggleDisposition(true, false, false),
			EDrawerToggleDisposition::FocusPanel);
		EXPECT_EQ(AccumulateConsoleUnreadImportantRecord(4, ELogLevel::Info), 4u);
		EXPECT_EQ(AccumulateConsoleUnreadImportantRecord(4, ELogLevel::Warn), 5u);
		EXPECT_EQ(AccumulateConsoleUnreadImportantRecord(998, ELogLevel::Error), 999u);
		EXPECT_EQ(AccumulateConsoleUnreadImportantRecord(999, ELogLevel::Fatal), 999u);
	}

	TEST(FEditorUILayoutTests, ClipsVariableHeightConsoleRecordsWithOverscan)
	{
		const std::array<float, 5> Offsets{0.0f, 20.0f, 60.0f, 80.0f, 140.0f};
		const FConsoleVisibleRange Middle = ResolveConsoleVisibleRange(Offsets, 55.0f, 30.0f);
		EXPECT_EQ(Middle.Begin, 0u);
		EXPECT_EQ(Middle.End, 4u);

		const FConsoleVisibleRange Bottom = ResolveConsoleVisibleRange(Offsets, 120.0f, 30.0f);
		EXPECT_EQ(Bottom.Begin, 2u);
		EXPECT_EQ(Bottom.End, 4u);
		EXPECT_EQ(ResolveConsoleVisibleRange({}, 0.0f, 100.0f).End, 0u);
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

	TEST(FMonaImGuiStyleTests, BottomDrawerAnimationAndGeometryStayBounded)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);
		MonaImGui::SetGlobalUIScale(1.0f);
		MonaImGui::FBottomDrawerState State;
		State.Open();
		MonaImGui::AdvanceBottomDrawerAnimation(State, 0.07f, 0.14f);
		EXPECT_FLOAT_EQ(State.Visibility, 0.5f);

		const MonaImGui::FBottomDrawerConfig Config{
			.Id = "TestDrawer",
			.AnchorMin = ImVec2(20.0f, 30.0f),
			.AnchorMax = ImVec2(1020.0f, 830.0f),
		};
		const MonaImGui::FBottomDrawerGeometry Geometry =
			MonaImGui::ResolveBottomDrawerGeometry(Config, State);
		EXPECT_FLOAT_EQ(Geometry.OpenHeight, 288.0f);
		EXPECT_FLOAT_EQ(Geometry.VisibleHeight, 144.0f);
		EXPECT_FLOAT_EQ(Geometry.Min.x, 20.0f);
		EXPECT_FLOAT_EQ(Geometry.Min.y, 686.0f);
		EXPECT_FLOAT_EQ(Geometry.Size.x, 1000.0f);
		EXPECT_FLOAT_EQ(Geometry.Size.y, 144.0f);

		State.HeightFraction = 10.0f;
		const MonaImGui::FBottomDrawerGeometry Clamped =
			MonaImGui::ResolveBottomDrawerGeometry(Config, State);
		EXPECT_FLOAT_EQ(Clamped.OpenHeight, 720.0f);
		for (const float Scale : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f})
		{
			MonaImGui::SetGlobalUIScale(Scale);
			const MonaImGui::FBottomDrawerGeometry Scaled =
				MonaImGui::ResolveBottomDrawerGeometry(Config, State);
			EXPECT_TRUE(std::isfinite(Scaled.VisibleHeight));
			EXPECT_GE(Scaled.OpenHeight, MonaImGui::ScaleUI(180.0f));
			EXPECT_LE(Scaled.OpenHeight, 800.0f);
		}
		MonaImGui::SetGlobalUIScale(1.0f);
		ImGui::DestroyContext(Context);
	}

	TEST(FMonaImGuiStyleTests, BottomDrawerInvalidTimingSettlesAtTarget)
	{
		MonaImGui::FBottomDrawerState State;
		State.Open();
		MonaImGui::AdvanceBottomDrawerAnimation(State, 0.0f, 0.14f);
		EXPECT_FLOAT_EQ(State.Visibility, 1.0f);
		State.Close();
		MonaImGui::AdvanceBottomDrawerAnimation(State, 0.1f, 0.0f);
		EXPECT_FLOAT_EQ(State.Visibility, 0.0f);
	}

	TEST(FMonaImGuiStyleTests, BottomDrawerDismissesOnlyAfterDragLeavesBounds)
	{
		const MonaImGui::FBottomDrawerConfig Config{
			.Id = "DragDismissDrawerTest",
			.bDismissWhenDragLeavesBounds = true,
		};
		const MonaImGui::FBottomDrawerGeometry Geometry{
			.Min = ImVec2(20.0f, 300.0f),
			.Size = ImVec2(1000.0f, 400.0f),
		};
		EXPECT_FALSE(MonaImGui::ShouldDismissBottomDrawerForDrag(
			Config, Geometry, false, ImVec2(100.0f, 200.0f)));
		EXPECT_FALSE(MonaImGui::ShouldDismissBottomDrawerForDrag(
			Config, Geometry, true, ImVec2(100.0f, 500.0f)));
		EXPECT_TRUE(MonaImGui::ShouldDismissBottomDrawerForDrag(
			Config, Geometry, true, ImVec2(100.0f, 299.0f)));
		EXPECT_TRUE(MonaImGui::ShouldDismissBottomDrawerForDrag(
			Config, Geometry, true, ImVec2(1020.0f, 500.0f)));
	}

	TEST(FMonaImGuiStyleTests, BottomDrawerBeginsAsTransientOverlay)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);
		ImGuiIO& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(1280.0f, 720.0f);
		IO.DeltaTime = 1.0f / 60.0f;
		IO.Fonts->Build();
		MonaImGui::FBottomDrawerState State;
		State.Open();
		const MonaImGui::FBottomDrawerConfig Config{
			.Id = "DrawerOverlayTest",
			.AnchorMin = ImVec2(10.0f, 20.0f),
			.AnchorMax = ImVec2(1010.0f, 620.0f),
			.AnimationDuration = 0.0f,
		};

		ImGui::NewFrame();
		ASSERT_TRUE(MonaImGui::BeginBottomDrawer(Config, State));
		EXPECT_FLOAT_EQ(ImGui::GetWindowPos().x, 10.0f);
		EXPECT_FLOAT_EQ(ImGui::GetWindowPos().y, 404.0f);
		EXPECT_FLOAT_EQ(ImGui::GetWindowSize().x, 1000.0f);
		EXPECT_FLOAT_EQ(ImGui::GetWindowSize().y, 216.0f);
		ImGui::TextUnformatted("Drawer contents");
		MonaImGui::EndBottomDrawer(State);
		ImGui::Render();

		State.Close();
		ImGui::NewFrame();
		EXPECT_FALSE(MonaImGui::BeginBottomDrawer(Config, State));
		ImGui::Render();
		ImGui::DestroyContext(Context);
	}

	TEST(FMonaImGuiStyleTests, TransientBottomDrawerClosesAfterLosingFocus)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);
		ImGuiIO& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(1280.0f, 720.0f);
		IO.DeltaTime = 1.0f / 60.0f;
		IO.Fonts->Build();
		MonaImGui::FBottomDrawerState State;
		State.Open();
		const MonaImGui::FBottomDrawerConfig Config{
			.Id = "FocusDismissDrawerTest",
			.AnchorMin = ImVec2(0.0f, 0.0f),
			.AnchorMax = ImVec2(1000.0f, 600.0f),
			.AnimationDuration = 0.0f,
			.bDismissOnFocusLoss = true,
		};

		ImGui::NewFrame();
		ASSERT_TRUE(MonaImGui::BeginBottomDrawer(Config, State));
		ImGui::TextUnformatted("Drawer contents");
		MonaImGui::EndBottomDrawer(State);
		ImGui::Begin("OtherWindow");
		ImGui::SetWindowFocus();
		ImGui::End();
		ImGui::Render();
		ASSERT_TRUE(State.IsOpen());

		ImGui::NewFrame();
		ASSERT_TRUE(MonaImGui::BeginBottomDrawer(Config, State));
		ImGui::TextUnformatted("Drawer contents");
		MonaImGui::EndBottomDrawer(State);
		ImGui::Render();
		EXPECT_FALSE(State.IsOpen());
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
