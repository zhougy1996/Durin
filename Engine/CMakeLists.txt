# Prepare third party dependencies
set(DOGE_PROJECT_THIRD_PARTY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Source/ThirdParty")
include(${CMAKE_CURRENT_SOURCE_DIR}/CMake/ThirdParty/ThirdParty.cmake)

doge_add_project(Engine)
