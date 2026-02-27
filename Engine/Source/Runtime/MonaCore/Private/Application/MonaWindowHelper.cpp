#include "MonaWindowHelper.h"

#include "Widgets/MWindow.h"

namespace Doge::Mona
{
	auto FMonaWindowHelper::FindWindowByPlatformWindow(const std::vector<TSharedPtr<MWindow>>& WindowsToSearch, TSharedPtr<FGenericWindow> PlatformWindow) -> TSharedPtr<MWindow>
	{
		for (const auto& window : WindowsToSearch)
		{
			if (window->GetNativeWindow() == PlatformWindow)
			{
				return window;
			}

			TSharedPtr<MWindow> FoundChildWindow = FindWindowByPlatformWindow(window->GetChildWindows(), PlatformWindow);

			if (FoundChildWindow)
			{
				return FoundChildWindow;
			}
		}

		return nullptr;
	}

	auto FMonaWindowHelper::ArrangeWindowToFront(std::vector<TSharedPtr<MWindow>>& Windows, TSharedPtr<MWindow> WindowToBringToFront) -> void
	{
		Windows.erase(std::remove(Windows.begin(), Windows.end(), WindowToBringToFront), Windows.end());
		Windows.push_back(WindowToBringToFront);
	}
}
