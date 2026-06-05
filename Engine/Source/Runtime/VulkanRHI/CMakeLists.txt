add_durin_module(VulkanRHI)

target_include_directories(VulkanRHI PRIVATE
    ${DURIN_PROJECT_SOURCE_DIR}/ThirdParty/VulkanMemoryAllocator/include
)

target_link_libraries(VulkanRHI PRIVATE
    Vulkan::Vulkan
)
