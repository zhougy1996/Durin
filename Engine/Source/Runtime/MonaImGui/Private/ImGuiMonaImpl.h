#pragma once

struct ImGuiContext;
struct ImGuiViewport;

namespace Durin
{
	class MWindow;
}

namespace Durin::Mona
{
	extern ImGuiContext* GMonaImGuiContext;

	auto ImGuiMonaImpl_Init() -> void;
	auto ImGuiMonaImpl_Shutdown() -> void;
	auto ImGuiMonaImpl_NewFrame() -> void;
	auto ImGuiMonaImpl_BindMainViewport(const std::shared_ptr<MWindow>& Window) -> void;
	auto ImGuiMonaImpl_GetViewportWindow(ImGuiViewport* Viewport) -> std::shared_ptr<MWindow>;
}
