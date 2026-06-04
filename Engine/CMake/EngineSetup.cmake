include_guard(GLOBAL)

get_filename_component(DURIN_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(_durin_root_dir "${DURIN_PROJECT_DIR}/.." ABSOLUTE)

include("${_durin_root_dir}/CMake/Common.cmake")
include("${_durin_root_dir}/CMake/Modules.cmake")

add_subdirectory("${DURIN_PROJECT_DIR}/CMake/ThirdParty")
