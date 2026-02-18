doge_add_module(VulkanRHI)

find_package(Vulkan)

target_link_libraries(VulkanRHI PRIVATE
    Vulkan::Vulkan
)


