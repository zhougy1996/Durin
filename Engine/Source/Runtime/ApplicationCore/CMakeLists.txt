doge_add_module(ApplicationCore)

include(${DOGE_PROJECT_CMAKE_DIR}/ThirdParty/glfw.cmake)
target_link_libraries(ApplicationCore PRIVATE
    Vulkan::Vulkan
    Doge_Glfw3
)


