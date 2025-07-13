project(Core)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(Core SHARED ${SRCS})
doge_setup_shared_library(Core)

target_include_directories(Core PRIVATE
    ${DOGE_THIRDPARTY_DIR}/spdlog
)

target_include_directories(Core PUBLIC
    ${DOGE_THIRDPARTY_DIR}/glm
    ${DOGE_THIRDPARTY_DIR}/rapidjson/include
)

target_link_libraries(Core PRIVATE 
    #spdlog::spdlog_header_only
    spdlog::spdlog
)


