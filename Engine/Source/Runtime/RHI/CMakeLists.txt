project(RHI)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(RHI SHARED ${SRCS})
doge_setup_shared_library(RHI)

target_link_libraries(RHI PRIVATE
    Core
)


