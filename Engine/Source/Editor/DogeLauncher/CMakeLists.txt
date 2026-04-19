doge_log_module(${project_name} "DogeLauncher")

add_executable(DogeLauncher "Private/Hello.cpp")
doge_set_module_output(DogeLauncher)

set_target_properties(DogeLauncher PROPERTIES OUTPUT_NAME "Doge")

target_link_libraries(DogeLauncher PRIVATE
    Launch
)

# Copy the config file to the output directory after building if it doesn't exist
set(config_file "$<TARGET_FILE_DIR:DogeLauncher>/DogeConfig.yaml")
add_custom_command(TARGET DogeLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E env
    ${CMAKE_COMMAND} -P "${DOGE_DIR}/CMake/CopyIfMissing.cmake"
    "${DOGE_PROJECT_CONFIG_DIR}/TP_DogeConfig.yaml" # template file
    ${config_file} # destination file
    VERBATIM
)



