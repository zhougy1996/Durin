#pragma once

#include "MonaAPI.h"
struct ImGuiContext;

namespace Doge::Mona
{
	MONA_API auto MonaInit() -> void;

	MONA_API auto MonaShutdown() -> void;

	MONA_API auto MonaUI_NewFrame() -> void;

	MONA_API auto MonaUI_Render() -> void;
}