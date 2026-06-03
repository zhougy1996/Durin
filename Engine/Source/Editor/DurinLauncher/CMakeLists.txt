durin_log_module(${project_name} "DurinLauncher")

add_executable(DurinLauncher "Private/Hello.cpp")
durin_set_module_output(DurinLauncher)

set_target_properties(DurinLauncher PROPERTIES OUTPUT_NAME "${DURIN_PROFILE_NAME}")

durin_apply_common_compile_definitions(DurinLauncher "DurinLauncher")

target_link_libraries(DurinLauncher PRIVATE
    Launch
)

# Remove the legacy config filename and copy the new one if it doesn't exist.
set(config_file "$<TARGET_FILE_DIR:DurinLauncher>/${DURIN_PROJECT_APP_CONFIG_FILE}")
set(legacy_config_file "$<TARGET_FILE_DIR:DurinLauncher>/${DURIN_PROJECT_PROFILE_NAME}Config.yaml")
add_custom_command(TARGET DurinLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f ${legacy_config_file}
    COMMAND ${CMAKE_COMMAND} -E env
    ${CMAKE_COMMAND} -P "${DURIN_DIR}/CMake/CopyIfMissing.cmake"
    "${DURIN_PROJECT_CONFIG_DIR}/TP_${DURIN_PROJECT_APP_CONFIG_FILE}" # template file
    ${config_file} # destination file
    VERBATIM
)



