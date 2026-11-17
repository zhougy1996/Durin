# Shared PCH for consumers of DurinEd's reflected types and editor services.

include_guard(GLOBAL)

add_durin_shared_pch(SharedPCH_DurinEd
    HEADER "${DURIN_PROJECT_ROOT_DIR}/Source/Editor/DurinEd/Public/DurinEdSharedPCH.h"
    LINK_LIBRARIES
        DurinEd
)

if(TARGET SharedPCH_DurinEd)
    # DurinEd supplies public include paths, including reflected generated headers.
    # DurinEd itself uses a private PCH with its own export definitions.
    add_dependencies(SharedPCH_DurinEd DurinEd_DHT)
endif()
