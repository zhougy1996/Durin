# Prepare third party dependencies
set(DOGE_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
add_subdirectory(${DOGE_PROJECT_DIR}/CMake/ThirdParty)

include(${DOGE_CMAKE_DIR}/Common.cmake)
include(${DOGE_CMAKE_DIR}/Modules.cmake)
set(DOGE_PROJECT_EXTERNAL_RUNTIME_DIR "${DOGE_PROJECT_DIR}/Binaries/ThirdParty/${DOGE_ARCH}/$<CONFIG>")
doge_add_project(Engine)
