#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	class FEngineLoop
	{
	public:
		auto PreInit(std::span<const std::string_view> Arguments = {}) -> void;
		auto Init() -> void;
		auto Tick() -> void;
		auto Exit() -> void;

	private:
		double LastTickTime = 0.0;
	};

	extern FEngineLoop GEngineLoop;
}
