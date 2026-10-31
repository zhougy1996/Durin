#pragma once

#include "CoreMinimal.h"

#include "Misc/CoreTypes.h"

namespace Durin
{
	struct FHostFrameState
	{
		bool bStartupReady = true;
		bool bEditor = false;
		bool bMinimized = false;
		bool bFocused = true;
		int32 FrameRateLimit = 0;
		int32 BackgroundFrameRateLimit = 30;
		bool bProfilerConnected = false;
	};

	// VSync remains a separate presentation policy.
	constexpr auto GetHostFrameInterval(const FHostFrameState& State) -> double
	{
		if (State.bMinimized) return State.bEditor ? 1.0 / 5.0 : 1.0 / 20.0;
		if (!State.bStartupReady) return 1.0 / 30.0;
		const double FrameInterval = State.FrameRateLimit > 0 ? 1.0 / State.FrameRateLimit : 0.0;
		if (State.bEditor && !State.bFocused && !State.bProfilerConnected && State.BackgroundFrameRateLimit > 0)
			return std::max(FrameInterval, 1.0 / State.BackgroundFrameRateLimit);
		return FrameInterval;
	}

	// Recompute from the last admitted frame, so policy changes do not retain an old
	// deadline and input wakeups cannot reset pacing. Bound waits to observe profiler
	// connections even when no native event wakes the host.
	constexpr auto GetHostFrameWaitTimeout(const FHostFrameState& State, double LastFrameTime, double CurrentTime) -> double
	{
		return std::clamp(LastFrameTime + GetHostFrameInterval(State) - CurrentTime, 0.0, 0.05);
	}
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
