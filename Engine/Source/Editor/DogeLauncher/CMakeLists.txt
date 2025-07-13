project(DogeLauncher)

doge_print_project_build_info()

add_executable(DogeLauncher "Private/Hello.cpp")
doge_set_module_output(DogeLauncher)

set_target_properties(DogeLauncher PROPERTIES OUTPUT_NAME "Doge")

target_link_libraries(DogeLauncher PRIVATE
    Launch
)


