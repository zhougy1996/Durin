include_guard(GLOBAL)

set(target_name Doge_spdlog)

set(SPDLOG_ROOT "${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/spdlog/spdlog-1.17.0")

add_library(${target_name} INTERFACE)

target_include_directories(${target_name} INTERFACE "${SPDLOG_ROOT}/include")