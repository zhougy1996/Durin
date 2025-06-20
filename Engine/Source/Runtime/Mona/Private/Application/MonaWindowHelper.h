#pragma once

class MWindow;
class FGenericWindow;

class FMonaWindowHelper
{
public:
	static auto FindWindowByPlatformWindow(const TArray<TSharedPtr<MWindow>>& WindowsToSearch, TSharedPtr<FGenericWindow> PlatformWindow) -> TSharedPtr<MWindow>;

	static auto ArrangeWindowToFront(TArray<TSharedPtr<MWindow>>& Windows, TSharedPtr<MWindow> WindowToBringToFront) -> void;
};