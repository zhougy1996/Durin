#include "MonaWindowHelper.h"

#include "Widgets/MWindow.h"

namespace Doge::Mona
{
	auto FMonaWindowHelper::FindWindowByPlatformWindow(const std::vector<std::shared_ptr<MWindow>>& WindowsToSearch, const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> std::shared_ptr<MWindow>
	{
		for (const auto& window : WindowsToSearch)
		{
			if (window->GetNativeWindow() == InPlatformWindow)
			{
				return window;
			}

			if (std::shared_ptr<MWindow> FoundChildWindow = FindWindowByPlatformWindow(window->GetChildWindows(), InPlatformWindow))
			{
				return FoundChildWindow;
			}
		}

		return nullptr;
	}

	auto FMonaWindowHelper::ArrangeWindowToFront(std::vector<std::shared_ptr<MWindow>>& Windows, const std::shared_ptr<MWindow>& WindowToBringToFront) -> void
	{
		Windows.erase(std::remove(Windows.begin(), Windows.end(), WindowToBringToFront), Windows.end());
		Windows.push_back(WindowToBringToFront);
	}
}
