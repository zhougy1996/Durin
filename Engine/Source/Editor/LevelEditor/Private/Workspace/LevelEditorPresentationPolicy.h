#pragma once

#include "Logging/Logger.h"

namespace Durin::Editor::Level
{
	// Classifies panel roles whose default visibility is part of the workspace layout contract.
	enum class ELevelEditorPanelRole : uint8
	{
		Persistent,
		Optional,
	};

	constexpr auto IsLevelEditorPanelOpenByDefault(
		ELevelEditorPanelRole Role) -> bool
	{
		return Role == ELevelEditorPanelRole::Persistent;
	}

} // namespace Durin::Editor::Level
