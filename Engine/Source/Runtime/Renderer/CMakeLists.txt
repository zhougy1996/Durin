project(Renderer)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(Renderer SHARED ${SRCS})
doge_setup_shared_library(Renderer)

target_link_libraries(Renderer PRIVATE
    Core
    RHI
    RenderCore
    Engine
)


