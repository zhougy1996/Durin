include_guard(GLOBAL)

include(FetchContent)

FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.zip
    URL_HASH SHA256=b11912a82d149792fef33fabd0503b13d54aeac25c1464755461d4108ea71fc2
    SOURCE_DIR "${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/spdlog"
)

set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)

set(CMAKE_MESSAGE_LOG_LEVEL NOTICE)
FetchContent_MakeAvailable(spdlog)
set(CMAKE_MESSAGE_LOG_LEVEL STATUS)