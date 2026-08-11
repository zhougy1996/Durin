#pragma once

#include "SceneView.h"
#include "ThirdParty/ImGui/imgui.h"

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
		ERenderMode RenderMode = ERenderMode::Lit;
		ERasterMode RasterMode = ERasterMode::Solid;
		std::string ViewModeLabel;
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
	};

	// Draws viewport toolbar controls without owning editor or viewport state.
	class FViewportToolbar final
	{
	public:
		auto CalculateLayout(const FLevelEditorViewportClient* ViewportClient, const FLevelViewportEditModeManager* EditModeManager, const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> FViewportToolbarLayout;
		auto Draw(
			FLevelEditorContext& Context,
			FLevelEditorViewportClient* ViewportClient,
			FLevelViewportEditModeManager* EditModeManager,
			::Durin::Editor::EPlayStartLocation& PreferredPlayStartLocation,
			::Durin::Editor::EPlayDestination& PreferredPlayDestination,
			const FViewportToolbarLayout& Layout
		) const -> void;
	};

	auto DrawViewportPlayStateBorder(const ImVec2& ViewportMin, const ImVec2& ViewportMax, bool bPaused) -> void;
	auto DrawViewportOrientationOverlay(const FLevelEditorViewportClient* ViewportClient, const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void;
	auto DrawViewportFPSOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void;
} // namespace Durin::Editor::Level
