#pragma once

#include "Panels/LevelEditorPanel.h"
#include "Widgets/MWidget.h"

struct ImVec2;

namespace Durin
{
	class MViewport;

	class FSceneViewportPanel final : public ILevelEditorPanel
	{
	public:
		FSceneViewportPanel();

		auto GetWindowName() const -> const char* override { return "Scene Viewport"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

		auto IsViewportHovered() const -> bool { return bViewportHovered; }
		auto IsViewportFocused() const -> bool { return bViewportFocused; }

	private:
		auto DrawToolbar() -> void;
		auto DrawOrientationOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> void;
		auto UpdateViewportSize() -> void;

		std::shared_ptr<MViewport> ViewportWidget;
		bool bViewportHovered = false;
		bool bViewportFocused = false;
	};
} // namespace Durin
