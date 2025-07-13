project(VulkanRHI)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(VulkanRHI SHARED ${SRCS})
doge_setup_shared_library(VulkanRHI)

find_package(Vulkan)

target_link_libraries(VulkanRHI PRIVATE
    Vulkan::Vulkan
    Core
    RHI
)


