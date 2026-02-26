#pragma once

namespace Doge
{
	class MWindow;
	class FGenericWindow;

	class FMonaWindowHelper
	{
	public:
		static auto FindWindowByPlatformWindow(const std::vector<TSharedPtr<MWindow>>& WindowsToSearch, TSharedPtr<FGenericWindow> PlatformWindow) -> TSharedPtr<MWindow>;

		static auto ArrangeWindowToFront(std::vector<TSharedPtr<MWindow>>& Windows, TSharedPtr<MWindow> WindowToBringToFront) -> void;
	};
}