doge_add_module(ApplicationCore)

target_link_libraries(ApplicationCore PRIVATE
    Vulkan::Vulkan
    glfw
)


