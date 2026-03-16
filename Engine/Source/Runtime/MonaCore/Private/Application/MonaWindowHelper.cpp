#include "MonaWindowHelper.h"

#include "Widgets/MWindow.h"

namespace Doge::Mona
{
	auto FMonaWindowHelper::FindWindowByPlatformWindow(const std::vector<std::shared_ptr<MWindow>>& WindowsToSearch, std::shared_ptr<FGenericWindow> PlatformWindow) -> std::shared_ptr<MWindow>
	{
		for (const auto& window : WindowsToSearch)
		{
			if (window->GetNativeWindow() == PlatformWindow)
			{
				return window;
			}

			std::shared_ptr<MWindow> FoundChildWindow = FindWindowByPlatformWindow(window->GetChildWindows(), PlatformWindow);

			if (FoundChildWindow)
			{
				return FoundChildWindow;
			}
		}

		return nullptr;
	}

	auto FMonaWindowHelper::ArrangeWindowToFront(std::vector<std::shared_ptr<MWindow>>& Windows, std::shared_ptr<MWindow> WindowToBringToFront) -> void
	{
		Windows.erase(std::remove(Windows.begin(), Windows.end(), WindowToBringToFront), Windows.end());
		Windows.push_back(WindowToBringToFront);
	}
}
