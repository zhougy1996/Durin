#pragma once

#include "LevelEditorAPI.h"
#include "Widgets/MCompoundWidget.h"

namespace Durin
{
	class MViewport;

	class MLevelEditor final : public MCompoundWidget
	{
	public:
		LEVELEDITOR_API auto Construct() -> void override;
		LEVELEDITOR_API auto Draw() -> void override;

	private:
		auto DrawViewportPanel() -> void;
		auto UpdateViewportSize() -> void;

		std::shared_ptr<MViewport> ViewportWidget;
	};
}
