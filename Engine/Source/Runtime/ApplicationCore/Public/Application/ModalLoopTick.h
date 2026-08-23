#pragma once

#include "ApplicationCoreAPI.h"

namespace Durin
{
	enum class EModalLoopTickMode
	{
		Opportunistic,
		Synchronized
	};

	using FModalLoopTickCallback = void (*)(EModalLoopTickMode Mode);

	// Installs the non-owning game-thread callback used by native modal loops.
	// Passing nullptr closes admission before Launch begins shutdown.
	APPLICATIONCORE_API auto SetModalLoopTickCallback(FModalLoopTickCallback Callback) -> void;

	// Requests one continuation frame. A missing callback is an intentional no-op.
	APPLICATIONCORE_API auto RequestModalLoopTick(EModalLoopTickMode Mode) -> void;
}
