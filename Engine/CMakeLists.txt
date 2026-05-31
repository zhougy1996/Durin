# Prepare third party dependencies
set(DURIN_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
add_subdirectory(${DURIN_PROJECT_DIR}/CMake/ThirdParty)

include(${DURIN_CMAKE_DIR}/Common.cmake)
include(${DURIN_CMAKE_DIR}/Modules.cmake)

include(${DURIN_PROJECT_DIR}/CMake/SharedPCH/SharedPCH_Core.cmake)
durin_add_project(Engine)
