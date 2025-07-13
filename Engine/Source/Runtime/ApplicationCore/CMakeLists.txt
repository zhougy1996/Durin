project(ApplicationCore)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(ApplicationCore SHARED ${SRCS})
doge_setup_shared_library(ApplicationCore)

find_package(Vulkan)

target_link_libraries(ApplicationCore PRIVATE
    Vulkan::Vulkan    
    glfw
    Core
)


