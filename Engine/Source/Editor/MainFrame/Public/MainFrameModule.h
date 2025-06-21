#pragma once

#include "Interfaces/IMainFrameModule.h"

class FMainFrameModule : public IMainFrameModule
{
public:
	FMainFrameModule() = default;
	~FMainFrameModule() = default;

	auto StartupModule() -> void override;
	auto ShutdownModule() -> void override;
	auto CreateDefaultMainFrame() -> void override;
};
