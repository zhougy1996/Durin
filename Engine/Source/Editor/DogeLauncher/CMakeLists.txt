message(STATUS "  Module: DogeLauncher")

add_executable(DogeLauncher "Private/Hello.cpp")
doge_set_module_output(DogeLauncher)

set_target_properties(DogeLauncher PROPERTIES OUTPUT_NAME "Doge")

target_link_libraries(DogeLauncher PRIVATE
    Launch
)

# Copy DogeConfig.json to the output directory if it doesn't exist
set(DEFAULT_DOGE_CONFIG "${DOGE_PROJECT_CONFIG_DIR}/DogeConfig.json")
set(TARGET_DOGE_CONFIG "$<TARGET_FILE_DIR:DogeLauncher>/DogeConfig.json")

add_custom_command(TARGET DogeLauncher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E compare_files "${TARGET_DOGE_CONFIG}" "${DEFAULT_DOGE_CONFIG}" || ${CMAKE_COMMAND} -E copy "${DEFAULT_DOGE_CONFIG}" "${TARGET_DOGE_CONFIG}"
        COMMENT "Preparing DogeConfig.json (copy if not exists or different)"
        VERBATIM
)



