# spdlog
add_subdirectory(spdlog)
set_target_properties(spdlog PROPERTIES FOLDER "ThirdParty/spdlog")

# glfw
set(GLFW_BUILD_DOCS OFF)
set(GLFW_INSTALL OFF)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_BUILD_TESTS OFF)
add_subdirectory(glfw)
set_target_properties(glfw PROPERTIES FOLDER "ThirdParty/GLFW3")
set_target_properties(update_mappings PROPERTIES FOLDER "ThirdParty/GLFW3")
