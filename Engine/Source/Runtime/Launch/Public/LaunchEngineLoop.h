#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Drives process-wide engine startup, ticking, and ordered shutdown.
	class FEngineLoop
	{
	public:
		auto PreInit(std::span<const std::string_view> Arguments = {}) -> void;
		auto Init() -> void;
		auto Tick() -> void;
		auto Exit() -> void;

	private:
		// Previous tick timestamp in the platform clock's seconds domain.
		double LastTickTime = 0.0;
	};

	extern FEngineLoop GEngineLoop;
}
