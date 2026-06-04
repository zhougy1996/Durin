include_guard(GLOBAL)

set(CORE_PUBLIC_DIR "${DURIN_PROJECT_DIR}/Source/Runtime/Core/Public")

durin_add_shared_pch_target(SharedPCH_Core
    HEADER "${CORE_PUBLIC_DIR}/CoreMinimal.h"
    INCLUDE_DIRECTORIES
        "${CORE_PUBLIC_DIR}"
    LINK_LIBRARIES
        glm::glm
)
