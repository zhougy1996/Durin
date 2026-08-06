durin_module_log(${DURIN_PROJECT_NAME} "DurinLauncher")

add_executable(DurinLauncher "Private/Hello.cpp")
durin_target_enable_windows_long_paths(DurinLauncher)
if(WIN32)
	target_sources(DurinLauncher PRIVATE "Resources/DurinEditor.rc")
endif()
durin_target_set_runtime_outputs(DurinLauncher)

set_target_properties(DurinLauncher PROPERTIES OUTPUT_NAME "${DURIN_RUNTIME_VARIANT}")

durin_target_apply_common_definitions(DurinLauncher "DurinLauncher")

target_link_libraries(DurinLauncher PRIVATE
    Launch
)

# Remove the oldest filename and preserve or create the config under Saved/Configs.
set(legacy_config_file_without_suffix "$<TARGET_FILE_DIR:DurinLauncher>/${DURIN_PROJECT_APP_CONFIG_FILE}")
set(legacy_config_file "$<TARGET_FILE_DIR:DurinLauncher>/${DURIN_PROJECT_RUNTIME_VARIANT}Config.yaml")
set(config_file "$<TARGET_FILE_DIR:DurinLauncher>/Saved/Configs/${DURIN_PROJECT_APP_CONFIG_FILE}")
add_custom_command(TARGET DurinLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f ${legacy_config_file}
    COMMAND ${CMAKE_COMMAND} -E env
    ${CMAKE_COMMAND} -P "${DURIN_WORKSPACE_DIR}/CMake/Utils/MigrateRuntimeConfigIfMissing.cmake"
    "${DURIN_PROJECT_CONFIG_DIR}/TP_${DURIN_PROJECT_APP_CONFIG_FILE}" # template file
    ${legacy_config_file_without_suffix} # previous destination
    ${config_file} # destination file
    VERBATIM
)
