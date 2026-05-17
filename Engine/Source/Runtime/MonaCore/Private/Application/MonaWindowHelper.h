#pragma once

namespace Durin
{
	class FGenericWindow;
}

namespace Durin::Mona
{
	class MWindow;

	class FMonaWindowHelper
	{
	public:
		static auto FindWindowByPlatformWindow(const std::vector<std::shared_ptr<MWindow>>& WindowsToSearch, const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> std::shared_ptr<MWindow>;

		static auto ArrangeWindowToFront(std::vector<std::shared_ptr<MWindow>>& Windows, const std::shared_ptr<MWindow>& WindowToBringToFront) -> void;
	};
}