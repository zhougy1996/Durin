#include "MonaWindowHelper.h"

#include "Widgets/MWindow.h"

namespace Durin::Mona
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

	auto FMonaWindowHelper::CollectWindowAndDescendants(const std::shared_ptr<MWindow>& Window, std::vector<std::shared_ptr<MWindow>>& OutWindows) -> void
	{
		if (Window == nullptr)
		{
			return;
		}

		OutWindows.push_back(Window);
		for (const std::shared_ptr<MWindow>& ChildWindow : Window->GetChildWindows())
		{
			CollectWindowAndDescendants(ChildWindow, OutWindows);
		}
	}

	auto FMonaWindowHelper::ArrangeWindowToFront(std::vector<std::shared_ptr<MWindow>>& Windows, const std::shared_ptr<MWindow>& WindowToBringToFront) -> void
	{
		if (WindowToBringToFront == nullptr)
		{
			return;
		}

		std::shared_ptr<MWindow> RootWindow = WindowToBringToFront;
		while (const std::shared_ptr<MWindow> ParentWindow = RootWindow->GetParentWindow())
		{
			RootWindow = ParentWindow;
		}

		std::vector<std::shared_ptr<MWindow>> WindowHierarchy;
		CollectWindowAndDescendants(RootWindow, WindowHierarchy);
		for (const std::shared_ptr<MWindow>& Window : WindowHierarchy)
		{
			Windows.erase(std::remove(Windows.begin(), Windows.end(), Window), Windows.end());
		}

		Windows.insert(Windows.end(), WindowHierarchy.begin(), WindowHierarchy.end());
	}
}
