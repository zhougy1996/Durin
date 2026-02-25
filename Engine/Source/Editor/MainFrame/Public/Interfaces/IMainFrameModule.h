#pragma once

class IMainFrameModule : public IModuleInterface
{
public:
	virtual auto CreateDefaultMainFrame() -> void = 0;
};