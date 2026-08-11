#pragma once

namespace Durin
{
	// Keeps platform input ahead of gameplay while leaving UI work after gameplay.
	template <typename TPumpPlatformEvents, typename TTickGame, typename TTickUI, typename TIsExitRequested>
	auto RunInteractiveFramePhases(
		TPumpPlatformEvents&& PumpPlatformEvents,
		TTickGame&& TickGame,
		TTickUI&& TickUI,
		TIsExitRequested&& IsExitRequested) -> bool
	{
		PumpPlatformEvents();
		if (IsExitRequested()) return false;
		TickGame();
		TickUI();
		return !IsExitRequested();
	}
}
