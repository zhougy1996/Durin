#include "EngineLoop/EngineLoop.h"

auto FEngineLoop::PreInit() -> void
{
	LoggerInit();
	DOGE_INFO("PreInit");
}

auto FEngineLoop::Init() -> void
{
}

auto FEngineLoop::Tick() -> void
{
}

auto FEngineLoop::Exit() -> void
{
}