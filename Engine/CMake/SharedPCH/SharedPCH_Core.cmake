# Shared PCH target owned by Core for engine/runtime modules.

include_guard(GLOBAL)

set(CORE_PUBLIC_DIR "${DURIN_PROJECT_ROOT_DIR}/Source/Runtime/Core/Public")

add_durin_shared_pch(SharedPCH_Core
    HEADER "${CORE_PUBLIC_DIR}/CoreSharedPCH.h"
    INCLUDE_DIRECTORIES
        "${CORE_PUBLIC_DIR}"
    LINK_LIBRARIES
        glm::glm
)
