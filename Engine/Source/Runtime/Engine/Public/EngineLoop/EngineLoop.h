#pragma once

#include "EngineDefinitions.h"

class ENGINE_API FEngineLoop
{
public:
	FEngineLoop() = default;
	~FEngineLoop() = default;

	auto PreInit() -> void;
	auto Init() -> void;
	auto Tick() -> void;
	auto Exit() -> void;
};

extern ENGINE_API FEngineLoop GEngineLoop;