#pragma once

#include "Logging/Logger.h"

namespace Durin::Editor::Level
{
	// Classifies panel roles whose default visibility is part of the workspace layout contract.
	enum class ELevelEditorPanelRole : uint8
	{
		Persistent,
		DrawerTool,
		ActivityHistory,
	};

	constexpr auto IsLevelEditorPanelOpenByDefault(
		ELevelEditorPanelRole Role) -> bool
	{
		return Role == ELevelEditorPanelRole::Persistent;
	}

	constexpr auto AccumulateConsoleUnreadImportantRecord(
		uint32 CurrentCount, ELogLevel Level) -> uint32
	{
		if (Level < ELogLevel::Warn) return CurrentCount;
		return CurrentCount >= 999 ? 999 : CurrentCount + 1;
	}
} // namespace Durin::Editor::Level
