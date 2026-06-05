# Workspace-level CMake setup entrypoint: compiler/toolchain defaults plus shared build API.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/Config/BuildOptions.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/Config/Toolchains.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/Utils/Timer.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DurinBuildApi.cmake")
