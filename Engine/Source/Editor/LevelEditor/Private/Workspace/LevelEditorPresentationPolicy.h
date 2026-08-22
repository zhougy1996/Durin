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

	enum class EDrawerToggleDisposition : uint8
	{
		OpenDrawer,
		CloseDrawer,
		FocusPanel,
	};

	constexpr auto IsLevelEditorPanelOpenByDefault(
		ELevelEditorPanelRole Role) -> bool
	{
		return Role == ELevelEditorPanelRole::Persistent;
	}

	constexpr auto ResolveDrawerToggleDisposition(
		bool bPanelOpen, bool bDrawerOpen, bool bSameTool) -> EDrawerToggleDisposition
	{
		if (bPanelOpen) return EDrawerToggleDisposition::FocusPanel;
		if (bDrawerOpen && bSameTool) return EDrawerToggleDisposition::CloseDrawer;
		return EDrawerToggleDisposition::OpenDrawer;
	}

	constexpr auto AccumulateConsoleUnreadImportantRecord(
		uint32 CurrentCount, ELogLevel Level) -> uint32
	{
		if (Level < ELogLevel::Warn) return CurrentCount;
		return CurrentCount >= 999 ? 999 : CurrentCount + 1;
	}
} // namespace Durin::Editor::Level
