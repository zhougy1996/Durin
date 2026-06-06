# Shared PCH target for editor modules built on Mona + ImGui.

include_guard(GLOBAL)

set(CORE_PUBLIC_DIR "${DURIN_PROJECT_ROOT_DIR}/Source/Runtime/Core/Public")
set(RHI_PUBLIC_DIR "${DURIN_PROJECT_ROOT_DIR}/Source/Runtime/RHI/Public")
set(MONAIMGUI_PUBLIC_DIR "${DURIN_PROJECT_ROOT_DIR}/Source/Runtime/MonaImGui/Public")

add_durin_shared_pch(SharedPCH_MonaEditor
    HEADER "${DURIN_PROJECT_ROOT_DIR}/CMake/SharedPCH/SharedPCH_MonaEditor.h"
    INCLUDE_DIRECTORIES
        "${CORE_PUBLIC_DIR}"
        "${RHI_PUBLIC_DIR}"
        "${MONAIMGUI_PUBLIC_DIR}"
    LINK_LIBRARIES
        glm::glm
)
