durin_module_log(${DURIN_PROJECT_NAME} "DurinLauncher")

add_executable(DurinLauncher "Private/Main.cpp")
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

# Deploy an immutable template; Launch creates the writable config on first startup.
set(config_template_directory "$<TARGET_FILE_DIR:DurinLauncher>/Templates")
set(config_template_file "${config_template_directory}/TP_${DURIN_PROJECT_APP_CONFIG_FILE}")
add_custom_command(TARGET DurinLauncher POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory ${config_template_directory}
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${DURIN_PROJECT_CONFIG_DIR}/TP_${DURIN_PROJECT_APP_CONFIG_FILE}"
		${config_template_file}
    VERBATIM
)
