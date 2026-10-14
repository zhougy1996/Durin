#include <gtest/gtest.h>

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Workspace/LevelEditorUILayout.h"
#include "Workspace/LevelEditorPresentationPolicy.h"
#include "MonaImGui.h"
#include "Runtime/MonaImGui/Private/Backend/ImGuiRHIDrawState.h"
#include "Runtime/MonaImGui/Private/Backend/ImGuiRHIBufferCapacity.h"

namespace Durin::Editor::Level
{
	static_assert(!std::is_copy_constructible_v<
		::Durin::Editor::ContentBrowser::FScopedExtensionRegistration>);
	static_assert(std::is_move_constructible_v<
		::Durin::Editor::ContentBrowser::FScopedExtensionRegistration>);
	TEST(FEditorUILayoutTests, SelectsResponsiveModeAtBoundaries)
	{
		EXPECT_EQ(ResolveEditorUILayout(-1.0f, 520.0f, 780.0f), EEditorUILayoutMode::Narrow);
		EXPECT_EQ(ResolveEditorUILayout(519.0f, 520.0f, 780.0f), EEditorUILayoutMode::Narrow);
		EXPECT_EQ(ResolveEditorUILayout(520.0f, 520.0f, 780.0f), EEditorUILayoutMode::Compact);
		EXPECT_EQ(ResolveEditorUILayout(779.0f, 520.0f, 780.0f), EEditorUILayoutMode::Compact);
		EXPECT_EQ(ResolveEditorUILayout(780.0f, 520.0f, 780.0f), EEditorUILayoutMode::Full);
	}

	TEST(FEditorUILayoutTests, AppliesViewportFirstPanelPolicy)
	{
		EXPECT_TRUE(IsLevelEditorPanelOpenByDefault(
			ELevelEditorPanelRole::Persistent));
		EXPECT_FALSE(IsLevelEditorPanelOpenByDefault(
			ELevelEditorPanelRole::Optional));
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

	TEST(FMonaImGuiStyleTests, DarkThemeKeepsChromeInteractionNeutral)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);
		MonaImGui::SetColorTheme(MonaImGui::EColorTheme::Dark);
		const ImGuiStyle& Style = ImGui::GetStyle();

		auto ChannelSpread = [](const ImVec4& Color) {
			return std::max({Color.x, Color.y, Color.z})
				- std::min({Color.x, Color.y, Color.z});
		};
		EXPECT_LT(ChannelSpread(Style.Colors[ImGuiCol_ButtonHovered]), 0.06f);
		EXPECT_LT(ChannelSpread(Style.Colors[ImGuiCol_HeaderHovered]), 0.06f);
		EXPECT_LT(ChannelSpread(Style.Colors[ImGuiCol_TabHovered]), 0.06f);
		EXPECT_LT(ChannelSpread(Style.Colors[ImGuiCol_TabSelectedOverline]), 0.08f);
		EXPECT_LT(ChannelSpread(Style.Colors[ImGuiCol_TabDimmedSelectedOverline]), 0.08f);
		EXPECT_LT(ChannelSpread(Style.Colors[ImGuiCol_CheckMark]), 0.06f);
		EXPECT_LT(ChannelSpread(Style.Colors[ImGuiCol_SliderGrabActive]), 0.08f);
		EXPECT_LT(ChannelSpread(Style.Colors[ImGuiCol_NavCursor]), 0.08f);
		EXPECT_GT(Style.Colors[ImGuiCol_ButtonHovered].x,
			Style.Colors[ImGuiCol_Button].x);
		EXPECT_GT(Style.Colors[ImGuiCol_ButtonActive].x,
			Style.Colors[ImGuiCol_ButtonHovered].x);

		const ImVec4& DockingPreview = Style.Colors[ImGuiCol_DockingPreview];
		EXPECT_LT(ChannelSpread(DockingPreview), 0.08f);
		EXPECT_GT(DockingPreview.x, Style.Colors[ImGuiCol_WindowBg].x);
		EXPECT_GT(DockingPreview.w, 0.5f);
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

	TEST(FMonaImGuiStyleTests, OpenBottomDrawerRearmsFocusForReplacementContents)
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
			.Id = "ReplacementDrawerTest",
			.AnchorMin = ImVec2(0.0f, 0.0f),
			.AnchorMax = ImVec2(1000.0f, 600.0f),
			.AnimationDuration = 0.0f,
			.bDismissOnFocusLoss = true,
		};

		ImGui::NewFrame();
		ASSERT_TRUE(MonaImGui::BeginBottomDrawer(Config, State));
		ImGui::TextUnformatted("Initial contents");
		MonaImGui::EndBottomDrawer(State);
		ImGui::Render();

		State.Open();
		ImGui::NewFrame();
		ImGui::Begin("DrawerSwitcher");
		ImGui::SetWindowFocus();
		ImGui::End();
		ASSERT_TRUE(MonaImGui::BeginBottomDrawer(Config, State));
		ImGui::TextUnformatted("Replacement contents");
		MonaImGui::EndBottomDrawer(State);
		ImGui::Render();
		EXPECT_TRUE(State.IsOpen());
		ImGui::DestroyContext(Context);
	}

	TEST(FMonaImGuiStyleTests, ExpandablePropertyRowsMatchFramedValueHeight)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);
		ImGuiIO& IO = ImGui::GetIO();
		IO.IniFilename = nullptr;
		IO.DisplaySize = ImVec2(800.0f, 600.0f);
		IO.DeltaTime = 1.0f / 60.0f;
		IO.Fonts->Build();

		ImGui::NewFrame();
		ImGui::Begin("ExpandablePropertyRowTest");
		ASSERT_TRUE(MonaImGui::PropertyEdit::BeginTable("ExpandablePropertyRows"));
		const bool bOpen = MonaImGui::PropertyEdit::BeginTreeRow("Struct", "Struct");
		const float TreeRowHeight = ImGui::GetItemRectSize().y;
		if (bOpen) MonaImGui::PropertyEdit::EndTreeRow();
		MonaImGui::PropertyEdit::EndTable();
		ImGui::End();
		ImGui::Render();

		EXPECT_FLOAT_EQ(TreeRowHeight, ImGui::GetFrameHeight());
		ImGui::DestroyContext(Context);
	}
} // namespace Durin::Editor::Level

TEST(FImGuiRHIDrawStateTests, RepeatedDrawsReuseBindingsAndStateChangesRemainIndependent)
{
	Durin::MonaImGui::FImGuiRHIDrawState State;
	ASSERT_TRUE(State.NeedsRenderStateSetup());
	State.OnRenderStateSetup();
	int Font = 0;
	int Image = 0;
	const std::array<float, 4> First{0, 0, 100, 100};
	const std::array<float, 4> Second{10, 20, 80, 70};
	uint32 Scissors = 0, Bindings = 0;
	for (uint32 Draw = 0; Draw < 100; ++Draw)
	{
		EXPECT_FALSE(State.NeedsRenderStateSetup());
		Scissors += State.ShouldSetScissor(First);
		Bindings += State.ShouldBindTexture(&Font);
	}
	EXPECT_EQ(Scissors, 1u);
	EXPECT_EQ(Bindings, 1u);
	EXPECT_TRUE(State.ShouldSetScissor(Second));
	EXPECT_FALSE(State.ShouldBindTexture(&Font));
	EXPECT_TRUE(State.ShouldBindTexture(&Image));
	EXPECT_FALSE(State.ShouldSetScissor(Second));
	EXPECT_TRUE(State.ShouldBindTexture(&Font));
}

TEST(FImGuiRHIDrawStateTests, CallbackInvalidationAndExplicitResetRebindIdenticalDrawState)
{
	Durin::MonaImGui::FImGuiRHIDrawState State;
	int Font = 0;
	const std::array<float, 4> Scissor{0, 0, 100, 100};
	State.OnRenderStateSetup();
	ASSERT_TRUE(State.ShouldSetScissor(Scissor));
	ASSERT_TRUE(State.ShouldBindTexture(&Font));
	State.Invalidate(); // An arbitrary user callback can change every binding.
	EXPECT_TRUE(State.NeedsRenderStateSetup());
	EXPECT_TRUE(State.NeedsRenderStateSetup()); // A clipped command must not consume invalidation.
	State.OnRenderStateSetup();
	EXPECT_TRUE(State.ShouldSetScissor(Scissor));
	EXPECT_TRUE(State.ShouldBindTexture(&Font));
	State.OnRenderStateSetup(); // ResetRenderState immediately restores common state.
	EXPECT_FALSE(State.NeedsRenderStateSetup());
	EXPECT_TRUE(State.ShouldSetScissor(Scissor));
	EXPECT_TRUE(State.ShouldBindTexture(&Font));
	Durin::MonaImGui::FImGuiRHIDrawState NextPass;
	EXPECT_TRUE(NextPass.NeedsRenderStateSetup());
	EXPECT_TRUE(NextPass.ShouldSetScissor(Scissor));
	EXPECT_TRUE(NextPass.ShouldBindTexture(&Font));
}

TEST(FImGuiRHIBufferCapacityTests, GradualGrowthReusesCapacityAndNeverShrinks)
{
	uint32 Capacity = 0;
	uint32 Allocations = 0;
	for (uint32 Count = 1; Count <= 10000; ++Count)
	{
		const uint32 Required = Count * 20;
		const uint32 Next = Durin::MonaImGui::ComputeImGuiBufferCapacity(Capacity, Required, 64 * 1024, 20);
		Allocations += Next != Capacity;
		EXPECT_GE(Next, Required);
		EXPECT_GE(Next, Capacity);
		EXPECT_EQ(Next % 20, 0u);
		Capacity = Next;
	}
	EXPECT_LT(Allocations, 10u);
	EXPECT_EQ(Durin::MonaImGui::ComputeImGuiBufferCapacity(Capacity, 20, 64 * 1024, 20), Capacity);
	EXPECT_EQ(Durin::MonaImGui::ComputeImGuiBufferCapacity(Capacity, 0, 64 * 1024, 20), Capacity);
}

TEST(FImGuiRHIBufferCapacityTests, InitialLargeRequestsHaveHeadroomAndNearLimitGrowthDoesNotWrap)
{
	const uint32 Large = Durin::MonaImGui::ComputeImGuiBufferCapacity(0, 100000, 16 * 1024, 4);
	EXPECT_GT(Large, 100000u);
	EXPECT_EQ(Large % 4, 0u);
	const uint32 Maximum = std::numeric_limits<uint32>::max() / 4 * 4;
	EXPECT_EQ(Durin::MonaImGui::ComputeImGuiBufferCapacity(Maximum - 4096, Maximum, 16 * 1024, 4), Maximum);
	const uint32 IndexCapacity = Durin::MonaImGui::ComputeImGuiBufferCapacity(0, 2, 16 * 1024, 2);
	EXPECT_GE(IndexCapacity, 16u * 1024);
	EXPECT_EQ(IndexCapacity % 2, 0u);
}
