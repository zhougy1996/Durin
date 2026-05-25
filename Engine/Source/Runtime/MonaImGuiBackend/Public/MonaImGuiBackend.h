#pragma once

namespace Durin::Mona
{
	class MWindow;
	extern ImGuiContext* GMonaImGuiContext;

	namespace FMonaImGuiBackend
	{
		auto Initialize() -> void;

		auto Shutdown() -> void;

		auto NewFrame() -> void;

		auto Render() -> void;

		auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;
	};
}
