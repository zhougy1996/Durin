include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/Timer.cmake)

set(DHT_DIR ${DURIN_DIR}/Engine/Source/Programs/DurinHeaderTool)
set(DHT_MAIN ${Python_EXECUTABLE} "${DHT_DIR}/main.py")

function(durin_log_project project_name)
	message(STATUS "[Durin] Project: ${project_name}")
endfunction()

function(durin_log_module project_name module_name)
	message(STATUS "[${project_name}] Module: ${module_name}")
endfunction()

# Collect module information for the project (Engine, User custom Game projects, etc.)
function(durin_add_project project_name)
	durin_log_project(${project_name})
	durin_start("Project_${project_name}")
	project(Engine)
	set(DURIN_PROJECT_INTERMEDIATE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Intermediate/Build/${DURIN_ARCH}/${DURIN_PROFILE_NAME}")

	set(project_cmake_file "${DURIN_PROJECT_INTERMEDIATE_BUILD_DIR}/${project_name}.project.cmake")
	execute_process(COMMAND ${DHT_MAIN} prepare_project_build -p ${project_name} -a ${DURIN_ARCH} --profile ${DURIN_PROFILE_NAME})
	include(${project_cmake_file})

	if(NOT DURIN_PROJECT_PROFILE_EXISTS)
		message(FATAL_ERROR "Project ${project_name} does not define profile ${DURIN_PROFILE_NAME}. Expected a dprofile file in ${DURIN_PROJECT_PROFILE_DIR}.")
	endif()

	if(DURIN_PROJECT_PROFILE_WITH_EDITOR)
		set(DURIN_WITH_EDITOR 1)
	else()
		set(DURIN_WITH_EDITOR 0)
	endif()
	set(DURIN_APP_CONFIG_NAME "${DURIN_PROJECT_PROFILE_APP_CONFIG_NAME}")
	set(DURIN_BIN_ROOT "${DURIN_PROJECT_BINARY_DIR}/${DURIN_ARCH}/$<CONFIG>")
	set(DURIN_RUNTIME_OUTPUT_DIR "${DURIN_BIN_ROOT}/Runtime/${DURIN_PROJECT_PROFILE_NAME}")
	set(DURIN_THIRDPARTY_RUNTIME_DIR "${DURIN_BIN_ROOT}/ThirdParty")
	set(DURIN_TEST_OUTPUT_DIR "${DURIN_BIN_ROOT}/Tests")
	set(DURIN_LIB_OUTPUT_ROOT "${DURIN_BIN_ROOT}/Lib")
	set(DURIN_SYMBOL_OUTPUT_ROOT "${DURIN_BIN_ROOT}/Symbols")
	set(DURIN_PROJECT_EXTERNAL_RUNTIME_DIR "${DURIN_THIRDPARTY_RUNTIME_DIR}")
	set(DURIN_BIN_ROOT "${DURIN_BIN_ROOT}" PARENT_SCOPE)
	set(DURIN_RUNTIME_OUTPUT_DIR "${DURIN_RUNTIME_OUTPUT_DIR}" PARENT_SCOPE)
	set(DURIN_THIRDPARTY_RUNTIME_DIR "${DURIN_THIRDPARTY_RUNTIME_DIR}" PARENT_SCOPE)
	set(DURIN_TEST_OUTPUT_DIR "${DURIN_TEST_OUTPUT_DIR}" PARENT_SCOPE)
	set(DURIN_LIB_OUTPUT_ROOT "${DURIN_LIB_OUTPUT_ROOT}" PARENT_SCOPE)
	set(DURIN_SYMBOL_OUTPUT_ROOT "${DURIN_SYMBOL_OUTPUT_ROOT}" PARENT_SCOPE)
	set(DURIN_PROJECT_EXTERNAL_RUNTIME_DIR "${DURIN_PROJECT_EXTERNAL_RUNTIME_DIR}" PARENT_SCOPE)

	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${project_config_file}) # Make CMake re-configure if the project definition file changes
	if(DURIN_PROJECT_PROFILE_EXISTS)
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DURIN_PROJECT_PROFILE_FILE})
	endif()

	# Add subdirectories for each module
	foreach(_dir IN LISTS project_module_dirs)
		add_subdirectory(${_dir})
	endforeach()
	durin_end()
endfunction()

# Module Setup Functions
function(durin_set_module_output target)
	set_target_properties(${target} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${DURIN_RUNTIME_OUTPUT_DIR}"
		LIBRARY_OUTPUT_DIRECTORY "${DURIN_RUNTIME_OUTPUT_DIR}"
		ARCHIVE_OUTPUT_DIRECTORY "${DURIN_LIB_OUTPUT_ROOT}/${target}"
		PDB_OUTPUT_DIRECTORY "${DURIN_SYMBOL_OUTPUT_ROOT}/${target}"
	)
endfunction()

function(durin_apply_common_compile_definitions target module_name)
	target_compile_definitions(${target} PRIVATE
		$<$<CONFIG:Debug>:DURIN_BUILD_DEBUG=1>
		$<$<CONFIG:Release>:DURIN_BUILD_RELEASE=1>
		DURIN_WITH_EDITOR=${DURIN_WITH_EDITOR}
		MODULE_NAME="${module_name}"
		DURIN_PROFILE_NAME="${DURIN_PROFILE_NAME}"
		DURIN_APP_CONFIG_NAME="${DURIN_APP_CONFIG_NAME}"
	)
endfunction()

function(durin_add_module module_name)
	durin_log_module(${project_name} ${module_name})
	durin_start("Module_${module_name}")

	set(module_cmake_file "${DURIN_PROJECT_INTERMEDIATE_BUILD_DIR}/${module_name}/${module_name}.module.cmake")
	include(${module_cmake_file})

	# Make CMake re-configure if the module definition file changes
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${module_config_file})

	if (module_reflect_headers)
		add_custom_command(
			OUTPUT ${module_export_file}
			COMMAND ${DHT_MAIN} generate_module_export_file -m ${module_name} -a ${DURIN_ARCH} --profile ${DURIN_PROFILE_NAME}
			DEPENDS ${module_reflect_headers} ${module_cmake_file}
		)

		add_custom_command(
			OUTPUT ${module_generated_srcs}
			COMMAND ${DHT_MAIN} generate_reflection_files -m ${module_name} -a ${DURIN_ARCH} --profile ${DURIN_PROFILE_NAME}
			DEPENDS ${module_cmake_file} ${module_manifest_dependencies} ${module_export_file}
		)
	endif()

	if("${module_link_type}" STREQUAL "STATIC")
		set(module_link_type_final STATIC)
	else()
		set(module_link_type_final SHARED)
	endif()

	add_library(${module_name} ${module_link_type_final})
	target_sources(${module_name} PUBLIC ${module_public_srcs} PRIVATE ${module_private_srcs} ${module_generated_srcs})

	set_target_properties(${module_name} PROPERTIES OUTPUT_NAME "${DURIN_PROFILE_NAME}-${module_name}")

	# Define export symbol for shared libraries, e.g. CORE_API
	if("${module_link_type_final}" STREQUAL "SHARED")
		string(TOUPPER ${module_name} module_name_upper)
		set_target_properties(${module_name} PROPERTIES DEFINE_SYMBOL "${module_name_upper}_EXPORTS")
	endif()

	durin_apply_common_compile_definitions(${module_name} ${module_name})

	target_include_directories(${module_name} PRIVATE
		${project_intermediate_build_dir}
		${CMAKE_CURRENT_SOURCE_DIR}/Private
	)

	target_include_directories(${module_name} PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/Public
		${module_dht_output_dir}
	)

	target_link_libraries(${module_name} PRIVATE ${module_private_dependencies})
	target_link_libraries(${module_name} PUBLIC ${module_public_dependencies})

	# Set up precompiled headers for the module
	if(module_pch_target)
		target_precompile_headers(${module_name} REUSE_FROM ${module_pch_target})
	else()
		target_precompile_headers(${module_name} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Private/PCH.${module_name}.h>")
	endif()

	durin_set_module_output(${module_name})
	# Organize the module in the IDE's folder structure
	set_target_properties(${module_name} PROPERTIES FOLDER "${project_name}/${module_dir}")

	durin_end() # ${module_name}
endfunction()

function(durin_add_test_target target_name)
	add_executable(${target_name} ${ARGN})

	durin_apply_common_compile_definitions(${target_name} ${target_name})

	if(TARGET SharedPCH_Core)
		target_precompile_headers(${target_name} REUSE_FROM SharedPCH_Core)
	endif()

	set_target_properties(${target_name} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${DURIN_TEST_OUTPUT_DIR}"
		LIBRARY_OUTPUT_DIRECTORY "${DURIN_TEST_OUTPUT_DIR}"
		ARCHIVE_OUTPUT_DIRECTORY "${DURIN_LIB_OUTPUT_ROOT}/${target_name}"
		PDB_OUTPUT_DIRECTORY "${DURIN_SYMBOL_OUTPUT_ROOT}/${target_name}"
	)
	set_target_properties(${target_name} PROPERTIES FOLDER "Tests/${target_name}")
endfunction()

function(durin_copy_external_target_binary target dependent_target)
	get_filename_component(file_name "$<TARGET_FILE_NAME:${dependent_target}>" NAME)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:${dependent_target}>"
		"${DURIN_PROJECT_EXTERNAL_RUNTIME_DIR}/${file_name}"
		COMMENT "Deploying target binary: ${file_name}"
		VERBATIM
	)
endfunction()

function(durin_copy_target_binary_to_output_dir target dependent_target)
	get_filename_component(file_name "$<TARGET_FILE_NAME:${dependent_target}>" NAME)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:${dependent_target}>"
		"$<TARGET_FILE_DIR:${target}>/${file_name}"
		COMMENT "Deploying target binary to output dir: ${file_name}"
		VERBATIM
	)
endfunction()

function(durin_copy_external_binaries_to_output_dir target file_list)
	foreach(file_path ${file_list})
		get_filename_component(file_name "${file_path}" NAME)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${file_path}"
			"$<TARGET_FILE_DIR:${target}>/${file_name}"
			COMMENT "Deploying external file to output dir: ${file_name}"
			VERBATIM
		)
	endforeach()
endfunction()

function(durin_copy_external_binaries target file_list)
	foreach(file_path ${file_list})
		get_filename_component(file_name "${file_path}" NAME)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${file_path}"
			"${DURIN_PROJECT_EXTERNAL_RUNTIME_DIR}/${file_name}"
			COMMENT "Deploying external file: ${file_name}"
			VERBATIM
		)
	endforeach()
endfunction()
