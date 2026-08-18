#pragma once

#include "MonaImGuiAPI.h"
#include "Window/GenericWindow.h"

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
	// Caches the cursor state last applied by the ImGui backend for one platform window.
	// Native non-client interactions may temporarily own the visible cursor, so the
	// backend must not rewrite an unchanged client cursor every frame.
	struct FMonaImGuiCursorState
	{
		EMouseCursor LastCursor = EMouseCursor::Count;
		ECursorMode LastMode = ECursorMode::Free;

		MONAIMGUI_API auto Reset() -> void;
		MONAIMGUI_API auto Apply(FGenericWindow& Window, EMouseCursor DesiredCursor, bool bDrawSoftwareCursor) -> void;
	};

	extern ImGuiContext* GMonaImGuiContext;

	auto ImGuiMonaImpl_Init() -> void;
	auto ImGuiMonaImpl_Shutdown() -> void;
	auto ImGuiMonaImpl_NewFrame() -> void;
	auto ImGuiMonaImpl_BindMainViewport(const std::shared_ptr<MWindow>& Window) -> void;
	auto ImGuiMonaImpl_GetViewportWindow(ImGuiViewport* Viewport) -> std::shared_ptr<MWindow>;
	MONAIMGUI_API auto ImGuiMonaImpl_CreateEventHandler() -> std::unique_ptr<Mona::FMonaEventHandler>;
}
