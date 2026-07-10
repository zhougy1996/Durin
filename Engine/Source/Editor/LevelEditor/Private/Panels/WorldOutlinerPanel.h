#pragma once

#include "Panels/LevelEditorPanel.h"

namespace Durin
{
	class FWorldOutlinerPanel final : public ILevelEditorPanel
	{
	public:
		auto GetWindowName() const -> const char* override { return "World Outliner"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		std::array<char, 128> SearchText{};
	};
} // namespace Durin
