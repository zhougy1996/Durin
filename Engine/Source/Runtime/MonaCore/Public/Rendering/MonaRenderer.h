#pragma once

class MWindow;

class MONA_CORE_API FMonaRenderer
{
public:
	virtual auto CreateViewport(const TSharedPtr<MWindow>& Window) -> void = 0;
};