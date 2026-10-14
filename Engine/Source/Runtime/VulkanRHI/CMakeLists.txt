add_durin_module(VulkanRHI)

option(DURIN_VULKAN_DETAILED_PROFILING "Enable high-frequency Vulkan CPU profiling zones" OFF)
target_compile_definitions(VulkanRHI PRIVATE
    DURIN_VULKAN_DETAILED_PROFILING=$<BOOL:${DURIN_VULKAN_DETAILED_PROFILING}>
)

target_link_libraries(VulkanRHI PRIVATE
    Vulkan::Vulkan
)
