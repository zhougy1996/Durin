#pragma once

#include "Panels/LevelEditorPanel.h"

namespace Durin
{
	struct FRenderGraphCapture;
	struct FSceneViewportStatisticsSnapshot;
}

namespace Durin::Editor::Level
{
	class FSceneViewportPanel;

	// Presents bounded viewport telemetry and explicit frame-graph captures.
	class FRenderingDiagnosticsPanel final : public ILevelEditorPanel
	{
	public:
		explicit FRenderingDiagnosticsPanel(FSceneViewportPanel& InViewportPanel);

		auto GetWindowName() const -> const char* override
		{
			return "Rendering Diagnostics";
		}
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		auto DrawOverview(
			const FSceneViewportStatisticsSnapshot& Snapshot,
			const FRenderGraphCapture* Capture) -> void;
		auto DrawScene(const FSceneViewportStatisticsSnapshot& Snapshot) -> void;
		auto DrawRenderGraph(const FRenderGraphCapture* Capture) -> void;
		auto RequestCapture() -> void;

		FSceneViewportPanel& ViewportPanel;
		uint64 ObservedCaptureRevision = 0;
		int32 SelectedPass = -1;
		bool bCapturePending = false;
		bool bFocusDependencies = true;
		float RenderGraphSidebarRatio = 0.27f;
		std::array<char, 128> PassFilter{};
	};
} // namespace Durin::Editor::Level
