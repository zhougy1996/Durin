#pragma once

class KWindow;
class FGenericWindow;

class FKleeWindowHelper
{
public:
	static auto FindWindowByPlatformWindow(const TArray<TSharedPtr<KWindow>>& WindowsToSearch, TSharedPtr<FGenericWindow> PlatformWindow) -> TSharedPtr<KWindow>;

	static auto ArrangeWindowToFront(TArray<TSharedPtr<KWindow>>& Windows, TSharedPtr<KWindow> WindowToBringToFront) -> void;
};