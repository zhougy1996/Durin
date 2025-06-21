#pragma once

struct ImGuiContext;

extern ImGuiContext* GImGuiContext;

inline MONA_API auto MonaInit() -> void;