#pragma once

class MAINFRAME_API IMainFrameModule : public IModuleInterface
{
public:
	virtual auto CreateDefaultMainFrame() -> void = 0;
};