#pragma once

struct ImGuiContext;

extern ImGuiContext* GImGuiContext;

MONA_API auto MonaInit() -> void;