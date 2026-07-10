#pragma once

#include "LevelEditorAPI.h"
#include "Widgets/MWidget.h"

struct ImVec2;

namespace Durin
{
	class MViewport;

	class MLevelEditor final : public MWidget
	{
	public:
		LEVELEDITOR_API auto Construct() -> void override;
		LEVELEDITOR_API auto Draw() -> void override;

	private:
		auto DrawViewportPanel() -> void;
		auto DrawViewportOrientationOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> void;
		auto UpdateViewportSize() -> void;

		std::shared_ptr<MViewport> ViewportWidget;
	};
}
