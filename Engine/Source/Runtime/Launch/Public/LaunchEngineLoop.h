#pragma once

namespace Doge
{
	class FEngineLoop
	{
	public:
		FEngineLoop() = default;
		~FEngineLoop() = default;

		auto PreInit() -> void;
		auto Init() -> void;
		auto Tick() -> void;
		auto Exit() -> void;
	};

	extern FEngineLoop GEngineLoop;
}