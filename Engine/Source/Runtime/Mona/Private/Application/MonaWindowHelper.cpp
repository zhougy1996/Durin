#include "MonaWindowHelper.h"

#include "Widgets/KWindow.h"

auto FKleeWindowHelper::FindWindowByPlatformWindow(const TArray<TSharedPtr<KWindow>>& WindowsToSearch, TSharedPtr<FGenericWindow> PlatformWindow) -> TSharedPtr<KWindow>
{
	for (const auto& window : WindowsToSearch)
	{
		if (window->GetNativeWindow() == PlatformWindow)
		{
			return window;
		}

		TSharedPtr<KWindow> FoundChildWindow = FindWindowByPlatformWindow(window->GetChildWindows(), PlatformWindow);

		if (FoundChildWindow)
		{
			return FoundChildWindow;
		}
	}

	return nullptr;
}

auto FKleeWindowHelper::ArrangeWindowToFront(TArray<TSharedPtr<KWindow>>& Windows, TSharedPtr<KWindow> WindowToBringToFront) -> void
{
	Windows.erase(std::remove(Windows.begin(), Windows.end(), WindowToBringToFront), Windows.end());
	Windows.push_back(WindowToBringToFront);
}
