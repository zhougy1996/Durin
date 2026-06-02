#pragma once
#include "MonaImGuiBackendAPI.h"

struct ImGuiContext;

namespace Durin::Mona
{
	class MWindow;
	extern MONAIMGUIBACKEND_API ImGuiContext* GMonaImGuiContext;

	namespace FMonaImGuiBackend
	{
		MONAIMGUIBACKEND_API auto Initialize() -> void;

		MONAIMGUIBACKEND_API auto Shutdown() -> void;

		MONAIMGUIBACKEND_API auto NewFrame() -> void;

		MONAIMGUIBACKEND_API auto Render() -> void;

		MONAIMGUIBACKEND_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;

		MONAIMGUIBACKEND_API auto ShowDemoWindow() -> void;
	};
}
