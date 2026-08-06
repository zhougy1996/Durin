add_durin_module(VulkanRHI)

target_link_libraries(VulkanRHI PRIVATE
    Vulkan::Vulkan
)

if(BUILD_TESTING)
	target_compile_definitions(VulkanRHI PRIVATE
		DURIN_VULKAN_TEST_FAILURE_INJECTION=1
	)
endif()
