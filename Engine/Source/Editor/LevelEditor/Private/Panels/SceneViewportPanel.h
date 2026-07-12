#pragma once

#include "Panels/LevelEditorPanel.h"
#include "Widgets/MWidget.h"

struct ImVec2;

namespace Durin
{
	class MViewport;
	class FLevelEditorViewportClient;
	class DLevel;
	struct FLevelViewportCameraState;

	class FSceneViewportPanel final : public ILevelEditorPanel
	{
	public:
		FSceneViewportPanel();
		~FSceneViewportPanel() override;

		auto GetWindowName() const -> const char* override { return "Scene Viewport"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

		auto IsViewportHovered() const -> bool { return bViewportHovered; }
		auto IsViewportFocused() const -> bool { return bViewportFocused; }
		auto CaptureCameraState(DLevel* Level, FLevelViewportCameraState& OutState) const -> bool;
		auto RestoreCameraState(DLevel* Level, const FLevelViewportCameraState* State) -> void;

	private:
		auto DrawToolbar(const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void;
		auto DrawOrientationOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> void;
		auto DrawFPSOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> void;
		auto UpdateViewportSize() -> void;
		auto UpdateViewportInput(FLevelEditorContext& Context) -> void;

		std::unique_ptr<FLevelEditorViewportClient> ViewportClient;
		std::shared_ptr<MViewport> ViewportWidget;
		bool bViewportHovered = false;
		bool bViewportFocused = false;
	};
} // namespace Durin
