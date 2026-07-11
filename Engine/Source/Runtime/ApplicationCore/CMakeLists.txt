add_durin_module(ApplicationCore)

target_link_libraries(ApplicationCore PRIVATE
    Vulkan::Vulkan
    glfw
)

if(WIN32)
	target_link_libraries(ApplicationCore PRIVATE ole32 shell32)
endif()

