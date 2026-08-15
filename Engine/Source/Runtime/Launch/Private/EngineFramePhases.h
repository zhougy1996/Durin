#pragma once

#include "Misc/CoreTypes.h"

namespace Durin
{
	// Identifies the game-thread frame boundary used to reject recursive continuations.
	enum class EInteractiveFrameState : uint8
	{
		Idle,
		PumpingPlatformEvents,
		RunningFrame,
		RunningModalFrame,
		ShuttingDown
	};

	// Runs a continuation only from inside the outer native event pump and restores
	// that state before the operating-system modal loop resumes dispatch.
	template <typename TRunFrame>
	auto TryRunModalContinuationFrame(
		EInteractiveFrameState& State,
		bool bAdmissionOpen,
		TRunFrame&& RunFrame) -> bool
	{
		if (!bAdmissionOpen || State != EInteractiveFrameState::PumpingPlatformEvents)
		{
			return false;
		}
		State = EInteractiveFrameState::RunningModalFrame;
		RunFrame();
		State = EInteractiveFrameState::PumpingPlatformEvents;
		return true;
	}

	// Keeps the outer native event pump ahead of the complete post-event frame.
	template <typename TPumpPlatformEvents, typename TRunFrame, typename TIsExitRequested>
	auto RunInteractiveFramePhases(
		EInteractiveFrameState& State,
		TPumpPlatformEvents&& PumpPlatformEvents,
		TRunFrame&& RunFrame,
		TIsExitRequested&& IsExitRequested) -> bool
	{
		State = EInteractiveFrameState::PumpingPlatformEvents;
		PumpPlatformEvents();
		if (IsExitRequested())
		{
			State = EInteractiveFrameState::Idle;
			return false;
		}
		State = EInteractiveFrameState::RunningFrame;
		RunFrame();
		const bool bContinue = !IsExitRequested();
		State = EInteractiveFrameState::Idle;
		return bContinue;
	}
}
