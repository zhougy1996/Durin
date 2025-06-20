#pragma once

struct ImGuiContext;

extern ImGuiContext* GImGuiContext;

inline KLEE_API auto GlfwInit() -> void;

inline KLEE_API auto KleeInit() -> void;