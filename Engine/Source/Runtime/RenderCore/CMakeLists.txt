project(RenderCore)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(RenderCore SHARED ${SRCS})
doge_setup_shared_library(RenderCore)

target_link_libraries(RenderCore PRIVATE
    Core
    RHI
)


