# Shared PCH target for modules that consume the stable DObject foundation.

include_guard(GLOBAL)

set(CORE_PUBLIC_DIR "${DURIN_PROJECT_ROOT_DIR}/Source/Runtime/Core/Public")
set(CORE_DOBJECT_PUBLIC_DIR "${DURIN_PROJECT_ROOT_DIR}/Source/Runtime/CoreDObject/Public")

add_durin_shared_pch(SharedPCH_DObject
    HEADER "${DURIN_PROJECT_ROOT_DIR}/CMake/SharedPCH/SharedPCH_DObject.h"
    INCLUDE_DIRECTORIES
        "${CORE_PUBLIC_DIR}"
        "${CORE_DOBJECT_PUBLIC_DIR}"
    LINK_LIBRARIES
        glm::glm
)
