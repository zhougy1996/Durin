#pragma once

#include "Logging/Logger.h"

namespace Durin::Editor::Level
{
	// Classifies panel roles whose default visibility is part of the workspace layout contract.
	enum class ELevelEditorPanelRole : uint8
	{
		Persistent,
		Optional,
		DrawerTool,
		ActivityHistory,
	};

	enum class EDrawerToggleDisposition : uint8
	{
		OpenDrawer,
		CloseDrawer,
		FocusPanel,
	};

	// Characterizes the legacy workspace visibility gate around shared-tool updates.
	enum class ELegacySharedToolUpdateDisposition : uint8
	{
		Skipped,
		Updated,
	};

	// Tracks single-instance tool submission without coupling the policy to ImGui.
	class FEditorToolFrameSubmissionState
	{
	public:
		constexpr auto TrySubmit(size_t ToolIndex, uint64 FrameSerial) -> bool
		{
			if (ToolIndex >= LastSubmittedFrame.size()) return false;
			if (LastSubmittedFrame[ToolIndex] == FrameSerial) return false;
			LastSubmittedFrame[ToolIndex] = FrameSerial;
			return true;
		}

	private:
		std::array<uint64, 2> LastSubmittedFrame{
			std::numeric_limits<uint64>::max(),
			std::numeric_limits<uint64>::max()};
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

	constexpr auto ResolveLegacySharedToolUpdateDisposition(bool bLevelRootVisible)
		-> ELegacySharedToolUpdateDisposition
	{
		return bLevelRootVisible
			? ELegacySharedToolUpdateDisposition::Updated
			: ELegacySharedToolUpdateDisposition::Skipped;
	}

	constexpr auto AccumulateConsoleUnreadImportantRecord(
		uint32 CurrentCount, ELogLevel Level) -> uint32
	{
		if (Level < ELogLevel::Warn) return CurrentCount;
		return CurrentCount >= 999 ? 999 : CurrentCount + 1;
	}
} // namespace Durin::Editor::Level
