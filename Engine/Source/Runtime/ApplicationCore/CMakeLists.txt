doge_add_module(ApplicationCore)

find_package(Vulkan)

target_link_libraries(ApplicationCore PRIVATE
    Vulkan::Vulkan    
    glfw
)


