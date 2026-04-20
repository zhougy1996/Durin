# Prepare third party dependencies
set(DOGE_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(ENGINE_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Source/ThirdParty")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/CMake/ThirdParty)

include(${DOGE_CMAKE_DIR}/Common.cmake)
include(${DOGE_CMAKE_DIR}/Modules.cmake)
doge_add_project(Engine)
