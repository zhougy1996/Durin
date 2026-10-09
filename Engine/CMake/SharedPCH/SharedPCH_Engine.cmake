# Shared PCH for modules that consume Engine's reflected types and RHI resources.

include_guard(GLOBAL)

add_durin_shared_pch(SharedPCH_Engine
    HEADER "${DURIN_PROJECT_ROOT_DIR}/CMake/SharedPCH/SharedPCH_Engine.h"
    LINK_LIBRARIES
        Engine
)

if(TARGET SharedPCH_Engine)
    # Engine's public usage requirements supply generated-header include paths.
    # Its reflected headers must exist before the PCH is compiled on a clean build.
    add_dependencies(SharedPCH_Engine Engine_DHT)
endif()
