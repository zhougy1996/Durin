# Aggregates all shared PCH targets declared by the Engine project.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/SharedPCH_Core.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/SharedPCH_DObject.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/SharedPCH_Engine.cmake")
if(DURIN_WITH_EDITOR)
    include("${CMAKE_CURRENT_LIST_DIR}/SharedPCH_DurinEd.cmake")
endif()
