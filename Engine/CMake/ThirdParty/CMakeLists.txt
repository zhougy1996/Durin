include_guard(GLOBAL)

set(DOGE_PROJECT_THIRD_PARTY_SOURCE_DIR "${ENGINE_THIRD_PARTY_DIR}")

message(DOGE_PROJECT_THIRD_PARTY_SOURCE_DIR=${DOGE_PROJECT_THIRD_PARTY_SOURCE_DIR})

include(${CMAKE_CURRENT_LIST_DIR}/spdlog.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/glfw.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/glm.cmake)
# include(${CMAKE_CURRENT_LIST_DIR}/glaze.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rapidyaml.cmake)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/assimp)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/slang)