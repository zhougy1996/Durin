#pragma once

#include "MonaImGuiAPI.h"

#include <memory>

struct ImGuiContext;
struct ImGuiViewport;

namespace Durin
{
	class MWindow;
	namespace Mona
	{
		class FMonaEventHandler;
	}
}

namespace Durin::MonaImGui
{
	extern ImGuiContext* GMonaImGuiContext;

	auto ImGuiMonaImpl_Init() -> void;
	auto ImGuiMonaImpl_Shutdown() -> void;
	auto ImGuiMonaImpl_NewFrame() -> void;
	auto ImGuiMonaImpl_BindMainViewport(const std::shared_ptr<MWindow>& Window) -> void;
	auto ImGuiMonaImpl_GetViewportWindow(ImGuiViewport* Viewport) -> std::shared_ptr<MWindow>;
	MONAIMGUI_API auto ImGuiMonaImpl_CreateEventHandler() -> std::unique_ptr<Mona::FMonaEventHandler>;
}
