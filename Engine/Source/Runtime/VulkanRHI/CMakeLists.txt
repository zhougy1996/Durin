add_durin_module(VulkanRHI)

target_link_libraries(VulkanRHI PRIVATE
    Vulkan::Vulkan
)
