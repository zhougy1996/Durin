#pragma once

namespace Durin
{
	class FGenericWindow;
	class MWindow;

	namespace Mona
	{
		class FMonaWindowHelper
		{
		public:
			static auto FindWindowByPlatformWindow(const std::vector<std::shared_ptr<MWindow>>& WindowsToSearch, const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> std::shared_ptr<MWindow>;

			static auto CollectWindowAndDescendants(const std::shared_ptr<MWindow>& Window, std::vector<std::shared_ptr<MWindow>>& OutWindows) -> void;

			static auto ArrangeWindowToFront(std::vector<std::shared_ptr<MWindow>>& Windows, const std::shared_ptr<MWindow>& WindowToBringToFront) -> void;
		};
	}
}


