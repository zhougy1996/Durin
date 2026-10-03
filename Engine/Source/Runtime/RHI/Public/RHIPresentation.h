#pragma once

#include "CoreMinimal.h"

#include "RHIAPI.h"

namespace Durin
{
	// Selects whether a viewport must pace the frame or may skip unavailable output.
	enum class EViewportPresentationPolicy : uint8
	{
		// Uses synchronized presentation and waits for output availability.
		FramePaced,
		// Uses low-latency presentation when available and bounds output waits.
		BestEffort,
		// Prefers immediate presentation, falling back to supported synchronized modes.
		Unsynchronized,
	};

	// Backend-neutral snapshot of the mode actually selected for a window.
	enum class EViewportPresentMode : uint8
	{
		Unavailable,
		Fifo,
		Mailbox,
		Immediate,
	};
}
