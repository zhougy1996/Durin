#pragma once

namespace Doge
{
	class FGenericWindow;
}

namespace Doge::Mona
{
	class MWindow;

	class FMonaWindowHelper
	{
	public:
		static auto FindWindowByPlatformWindow(const std::vector<std::shared_ptr<MWindow>>& WindowsToSearch, const std::shared_ptr<FGenericWindow>& PlatformWindow) -> std::shared_ptr<MWindow>;

		static auto ArrangeWindowToFront(std::vector<std::shared_ptr<MWindow>>& Windows, const std::shared_ptr<MWindow>& WindowToBringToFront) -> void;
	};
}