#pragma once

#include "MonaImGuiBackendAPI.h"

struct ImGuiContext;

namespace Durin::Mona
{
    extern MONAIMGUIBACKEND_API ImGuiContext* GMonaImGuiContext;
}

#ifndef IMGUI_API
    #define IMGUI_API MONAIMGUIBACKEND_API
#endif

#ifndef IMGUI_IMPL_API
    #define IMGUI_IMPL_API MONAIMGUIBACKEND_API
#endif

#ifndef IMGUI_DEFINE_MATH_OPERATORS
    #define IMGUI_DEFINE_MATH_OPERATORS
#endif

#ifndef GImGui
    #define GImGui Durin::Mona::GMonaImGuiContext
#endif
