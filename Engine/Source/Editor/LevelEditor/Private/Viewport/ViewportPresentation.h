#pragma once

#include "SceneView.h"
#include "MonaImGui.h"
#include "ThirdParty/ImGui/imgui.h"

namespace Durin
{
	class FViewportClient;
	struct FSceneViewportStatisticsSnapshot;
}

namespace Durin::Editor
{
	enum class EPlayStartLocation : uint8;
	enum class EPlayDestination : uint8;
}

namespace Durin::Editor::Level
{
	class FLevelEditorViewportClient;
	class FLevelViewportEditModeManager;
	struct FLevelEditorContext;

	// Retains responsive toolbar group placement for the current viewport size.
	struct FViewportToolbarLayout
	{
		bool bEnableFXAA = true;
		bool bEnableGroundTruthAmbientOcclusion = true;
		EGroundTruthAmbientOcclusionQuality GroundTruthAmbientOcclusionQuality =
			EGroundTruthAmbientOcclusionQuality::HalfResolution;
		ERenderMode RenderMode = ERenderMode::Lit;
		ERasterMode RasterMode = ERasterMode::Solid;
		std::string EditModeLabel;
		ImVec2 ViewportMin;
		ImVec2 ViewportMax;
		ImVec2 BackgroundMin;
		ImVec2 BackgroundMax;
		ImVec2 PlayBackgroundMin;
		ImVec2 PlayBackgroundMax;
		ImVec2 ViewModeButtonPosition;
		ImVec2 ViewModeButtonSize;
		ImVec2 PlayButtonPosition;
		float Height = 0.0f;
		float Gap = 0.0f;
		float ToolButtonGap = 0.0f;
		float ModeButtonWidth = 0.0f;
		float EditModeButtonWidth = 0.0f;
		float SpaceButtonWidth = 0.0f;
		float SnapButtonWidth = 0.0f;
		float DropDownWidth = 0.0f;
		float PlayButtonWidth = 0.0f;
		float RuntimeButtonWidth = 0.0f;
		bool bCompact = false;
		bool bOverflow = false;
		bool bRenderSettingsTargetIsPlayWindow = false;
	};

	// Describes the FPS badge and optional statistics panel within one viewport.
	struct FViewportStatisticsOverlayLayout
	{
		ImVec2 BadgeMin;
		ImVec2 BadgeMax;
		ImVec2 PanelMin;
		ImVec2 PanelMax;
		bool bPanelVisible = false;

		auto Contains(const ImVec2& Position) const -> bool
		{
			const bool bInsideBadge = Position.x >= BadgeMin.x
				&& Position.x <= BadgeMax.x && Position.y >= BadgeMin.y
				&& Position.y <= BadgeMax.y;
			const bool bInsidePanel = bPanelVisible && Position.x >= PanelMin.x
				&& Position.x <= PanelMax.x && Position.y >= PanelMin.y
				&& Position.y <= PanelMax.y;
			return bInsideBadge || bInsidePanel;
		}
	};

	// Separates non-mutating render inspection from editor camera and scene mutation authority.
	struct FViewportToolbarCapabilities
	{
		bool bCanEditCamera = false;
		bool bCanEditScene = false;
		bool bCanEditRenderSettings = true;
		bool bCanToggleCollision = false;
		bool bTargetsPlayWindow = false;
	};

	inline auto ResolveViewportToolbarCapabilities(
		bool bReadOnly,
		bool bPlaying,
		bool bPlayingInNewWindow) -> FViewportToolbarCapabilities
	{
		const bool bCanMutateEditor = !bReadOnly && !bPlaying;
		return {
			.bCanEditCamera = bCanMutateEditor,
			.bCanEditScene = bCanMutateEditor,
			.bCanEditRenderSettings = true,
			.bCanToggleCollision = !bReadOnly || bPlaying,
			.bTargetsPlayWindow = bPlaying && bPlayingInNewWindow,
		};
	}

	// The embedded viewport displays the gameplay camera during PIE, so editor-camera
	// orientation must not be overlaid on that image. A detached PIE session leaves
	// the editor viewport presenting its own editor view.
	inline auto ShouldDrawViewportOrientationOverlay(
		bool bPlaying,
		bool bPlayingInNewWindow) -> bool
	{
		return !bPlaying || bPlayingInNewWindow;
	}

	inline auto FormatViewportStatistic(uint64 Value) -> std::string
	{
		if (Value < 1000) return std::to_string(Value);
		if (Value < 1'000'000)
			return std::format("{:.1f}K", static_cast<double>(Value) / 1000.0);
		if (Value < 1'000'000'000)
			return std::format("{:.2f}M", static_cast<double>(Value) / 1'000'000.0);
		return std::format("{:.2f}B", static_cast<double>(Value) / 1'000'000'000.0);
	}

	inline auto GetViewportHudSurfaceOutset() -> float
	{
		return MonaImGui::ScaleUI(3.0f);
	}

	inline auto CalculateViewportStatisticsOverlayLayout(
		const ImVec2& ViewportMin,
		const ImVec2& ViewportMax,
		bool bExpanded) -> FViewportStatisticsOverlayLayout
	{
		char FpsText[32];
		snprintf(FpsText, sizeof(FpsText), "%.0f FPS", ImGui::GetIO().Framerate);
		const ImVec2 TextSize = ImGui::CalcTextSize(FpsText);
		const float OverlayHeight = std::max(
			MonaImGui::ScaleUI(30.0f),
			ImGui::GetFontSize() + MonaImGui::ScaleUI(12.0f));
		const float HorizontalPadding = MonaImGui::ScaleUI(7.0f);
		FViewportStatisticsOverlayLayout Layout;
		Layout.BadgeMax = ImVec2(
			ViewportMax.x - MonaImGui::ScaleUI(10.0f),
			ViewportMin.y + MonaImGui::ScaleUI(8.0f) + OverlayHeight);
		Layout.BadgeMin = ImVec2(
			Layout.BadgeMax.x - TextSize.x - HorizontalPadding * 2.0f,
			ViewportMin.y + MonaImGui::ScaleUI(8.0f));

		const float Margin = MonaImGui::ScaleUI(10.0f);
		const float Gap = MonaImGui::ScaleUI(5.0f);
		// The FPS button sits inside the top-right HUD surface, whose border
		// extends three scaled pixels beyond the button's interaction bounds.
		// Anchor the panel to that visible border so the two surfaces align.
		const float AvailableWidth = ViewportMax.x - ViewportMin.x - Margin * 2.0f;
		const float PanelWidth = std::min(MonaImGui::ScaleUI(248.0f), AvailableWidth);
		const float PanelHeight = MonaImGui::ScaleUI(395.0f);
		Layout.PanelMax = ImVec2(
			Layout.BadgeMax.x + GetViewportHudSurfaceOutset(),
			Layout.BadgeMax.y + Gap + PanelHeight);
		Layout.PanelMin = ImVec2(
			Layout.PanelMax.x - PanelWidth, Layout.BadgeMax.y + Gap);
		Layout.bPanelVisible = bExpanded
			&& PanelWidth >= MonaImGui::ScaleUI(190.0f)
			&& Layout.PanelMin.x >= ViewportMin.x + Margin
			&& Layout.PanelMax.y <= ViewportMax.y - Margin;
		return Layout;
	}

	// Draws viewport toolbar controls without owning editor or viewport state.
	class FViewportToolbar final
	{
	public:
		auto CalculateLayout(
			const FViewportClient* RenderSettingsClient,
			bool bRenderSettingsTargetIsPlayWindow,
			const FLevelViewportEditModeManager* EditModeManager,
			const ImVec2& ViewportMin,
			const ImVec2& ViewportMax) const -> FViewportToolbarLayout;
		auto Draw(
			FLevelEditorContext& Context,
			FLevelEditorViewportClient* EditorViewportClient,
			FViewportClient* RenderSettingsClient,
			FLevelViewportEditModeManager* EditModeManager,
			::Durin::Editor::EPlayStartLocation& PreferredPlayStartLocation,
			::Durin::Editor::EPlayDestination& PreferredPlayDestination,
			const FViewportToolbarLayout& Layout
		) const -> void;
	};

	auto DrawViewportPlayStateBorder(const ImVec2& ViewportMin, const ImVec2& ViewportMax, bool bPaused) -> void;
	auto DrawViewportOrientationOverlay(const FLevelEditorViewportClient* ViewportClient, const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void;
	auto DrawViewportCameraSpeedOverlay(const FLevelEditorViewportClient* ViewportClient, const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void;
	auto DrawViewportStatisticsOverlay(
		const ImVec2& ViewportMin,
		const ImVec2& ViewportMax,
		const FSceneViewportStatisticsSnapshot& Snapshot,
		bool& bExpanded) -> FViewportStatisticsOverlayLayout;
} // namespace Durin::Editor::Level
