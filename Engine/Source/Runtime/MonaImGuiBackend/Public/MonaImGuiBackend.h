#pragma once
#include "IMonaUIBackend.h"

namespace Durin::Mona
{
	class MWindow;
	extern ImGuiContext* GMonaImGuiContext;

	class FMonaImGuiUIBackend final : public IMonaUIBackend
	{
	public:
		auto Initialize() -> void override;

		auto Shutdown() -> void override;

		auto NewFrame() -> void override;

		auto Render() -> void override;

		auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void override;

		auto ShowDemoWindow() -> void override;
	};

	namespace FMonaImGuiBackend
	{
		auto Initialize() -> void;

		auto Shutdown() -> void;

		auto NewFrame() -> void;

		auto Render() -> void;

		auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;

		auto ShowDemoWindow() -> void;
	};
}
