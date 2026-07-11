#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	class FEngineLoop
	{
	public:
		auto PreInit(int ArgC, char** ArgV) -> void;
		auto Init() -> void;
		auto Tick() -> void;
		auto Exit() -> void;
	};

	extern FEngineLoop GEngineLoop;
}
