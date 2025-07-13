project(MonaCore)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(MonaCore SHARED ${SRCS})
doge_setup_shared_library(MonaCore)

target_link_libraries(MonaCore PRIVATE
    Core
    RHI
    ApplicationCore
    RenderCore
)


