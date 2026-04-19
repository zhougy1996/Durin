include_guard(GLOBAL)

include(FetchContent)

FetchContent_Declare(
    glfw
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
    URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
    SOURCE_DIR "${DOGE_PROJECT_THIRD_PARTY_SOURCE_DIR}/glfw"
)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(CMAKE_MESSAGE_LOG_LEVEL NOTICE)
FetchContent_MakeAvailable(glfw)
set(CMAKE_MESSAGE_LOG_LEVEL STATUS)
