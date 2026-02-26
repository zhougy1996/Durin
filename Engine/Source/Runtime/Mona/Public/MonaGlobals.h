#pragma once

struct ImGuiContext;

namespace Doge
{
	extern ImGuiContext* GImGuiContext;

	MONA_API auto MonaInit() -> void;
}