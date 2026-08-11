#pragma once

#include "DObject/Class.h"
#include "MonaImGui.h"

namespace Durin::Editor::Level::Helpers
{
	inline auto ClassDisplayName(const DClass* Class) -> std::string
	{
		return Class ? Class->GetDisplayName() : std::string();
	}

	inline auto DrawToolbarIconButton(const char* Icon, const char* Id) -> bool
	{
		return MonaImGui::ToolbarIconButton(Icon, Id);
	}
} // namespace Durin::Editor::Level::Helpers
