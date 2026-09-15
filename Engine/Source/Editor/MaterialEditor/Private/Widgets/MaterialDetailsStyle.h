#pragma once

#include "MonaImGuiPropertyTable.h"

namespace Durin::Editor::Material::DetailsStyle
{
	inline auto MakeTableConfig() -> MonaImGui::PropertyEdit::FTableConfig
	{
		MonaImGui::PropertyEdit::FTableConfig Config;
		Config.MaximumValueColumnWidthInEm = 34.0f;
		Config.CellPadding = {4.0f, 4.0f};
		return Config;
	}

	// Keep labels out of value controls and scope their hidden IDs to the row.
	template <typename FDraw>
	auto EditRow(const char* Label, FDraw&& Draw) -> bool
	{
		ImGui::PushID(Label);
		MonaImGui::PropertyEdit::BeginRow(Label);
		const bool bChanged = Draw();
		MonaImGui::PropertyEdit::EndRow();
		ImGui::PopID();
		return bChanged;
	}
}
