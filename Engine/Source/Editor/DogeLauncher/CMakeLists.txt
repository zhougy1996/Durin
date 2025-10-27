project(DogeLauncher)

doge_print_project_build_info()

add_executable(DogeLauncher "Private/Hello.cpp")
doge_set_module_output(DogeLauncher)

set_target_properties(DogeLauncher PROPERTIES OUTPUT_NAME "Doge")

target_link_libraries(DogeLauncher PRIVATE
    Launch
)

# Copy DogeConfig.json to the output directory if it doesn't exist
set(DEFAULT_DOGE_CONFIG "${DOGE_CONFIG_DIR}/DogeConfig.json")
set(TARGET_DOGE_CONFIG "$<TARGET_FILE_DIR:DogeLauncher>/DogeConfig.json")

add_custom_command(TARGET DogeLauncher POST_BUILD
    COMMAND if not exist "${TARGET_DOGE_CONFIG}" (
        ${CMAKE_COMMAND} -E copy
        "${DEFAULT_DOGE_CONFIG}"
        "${TARGET_DOGE_CONFIG}"
    )
    COMMENT "Preparing DogeConfig.json"
    VERBATIM
)



