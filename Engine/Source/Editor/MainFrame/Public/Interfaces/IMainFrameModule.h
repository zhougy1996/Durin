#pragma once

class MAIN_FRAME_API IMainFrameModule : public IModuleInterface
{
public:
	virtual auto CreateDefaultMainFrame() -> void = 0;
};