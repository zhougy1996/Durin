durin_log_module(${project_name} "DurinLauncher")

add_executable(DurinLauncher "Private/Hello.cpp")
durin_set_module_output(DurinLauncher)

set_target_properties(DurinLauncher PROPERTIES OUTPUT_NAME "${DURIN_PROFILE_NAME}")

durin_apply_common_compile_definitions(DurinLauncher "DurinLauncher")

target_link_libraries(DurinLauncher PRIVATE
    Launch
)

# Copy the config file to the output directory after building if it doesn't exist
set(config_file "$<TARGET_FILE_DIR:DurinLauncher>/${DURIN_APP_CONFIG_NAME}")
add_custom_command(TARGET DurinLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E env
    ${CMAKE_COMMAND} -P "${DURIN_DIR}/CMake/CopyIfMissing.cmake"
    "${DURIN_PROJECT_CONFIG_DIR}/TP_DurinConfig.yaml" # template file
    ${config_file} # destination file
    VERBATIM
)



