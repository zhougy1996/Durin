# Engine project entry script: establishes project paths and registers Engine-specific setup.

include_guard(GLOBAL)

get_filename_component(DURIN_PROJECT_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(DURIN_PROJECT_DIR "${DURIN_PROJECT_ROOT_DIR}")
get_filename_component(_durin_workspace_dir "${DURIN_PROJECT_ROOT_DIR}/.." ABSOLUTE)

include("${_durin_workspace_dir}/CMake/DurinWorkspaceSetup.cmake")

add_subdirectory("${DURIN_PROJECT_ROOT_DIR}/CMake/ThirdParty")
