durin_module_log(${DURIN_PROJECT_NAME} "DurinLauncher")

add_executable(DurinLauncher "Private/Hello.cpp")
if(WIN32)
	target_sources(DurinLauncher PRIVATE "Resources/DurinEditor.rc")
endif()
durin_target_set_runtime_outputs(DurinLauncher)

set_target_properties(DurinLauncher PROPERTIES OUTPUT_NAME "${DURIN_PROFILE_NAME}")

durin_target_apply_common_definitions(DurinLauncher "DurinLauncher")

target_link_libraries(DurinLauncher PRIVATE
    Launch
)

# Remove the legacy config filename and copy the new one if it doesn't exist.
set(config_file "$<TARGET_FILE_DIR:DurinLauncher>/${DURIN_PROJECT_APP_CONFIG_FILE}")
set(legacy_config_file "$<TARGET_FILE_DIR:DurinLauncher>/${DURIN_PROJECT_PROFILE_NAME}Config.yaml")
add_custom_command(TARGET DurinLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f ${legacy_config_file}
    COMMAND ${CMAKE_COMMAND} -E env
    ${CMAKE_COMMAND} -P "${DURIN_WORKSPACE_DIR}/CMake/Utils/CopyIfMissing.cmake"
    "${DURIN_PROJECT_CONFIG_DIR}/TP_${DURIN_PROJECT_APP_CONFIG_FILE}" # template file
    ${config_file} # destination file
    VERBATIM
)


