#pragma once

#include "MonaAPI.h"
struct ImGuiContext;

namespace Durin::Mona
{
	class MWindow;

	MONA_API auto MonaInit() -> void;

	MONA_API auto MonaShutdown() -> void;

	MONA_API auto NewFrame() -> void;

	MONA_API auto Render() -> void;

	MONA_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;

	MONA_API auto ShowDemoWindow() -> void;
}
