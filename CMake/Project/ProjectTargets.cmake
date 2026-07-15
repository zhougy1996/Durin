# Project target creation helpers for modules and native tests.

include_guard(GLOBAL)

function(durin_module_log project_name module_name)
	message(STATUS "[${project_name}] Module: ${module_name}")
endfunction()

function(durin_target_force_include_pch target_name header_path)
	if(NOT DURIN_FORCE_INCLUDE_PCH)
		return()
	endif()

	if(MSVC)
		target_compile_options(${target_name} PRIVATE
			"$<$<COMPILE_LANGUAGE:CXX>:/FI${header_path}>"
		)
	endif()
endfunction()

function(add_durin_module module_name)
	durin_module_log(${DURIN_PROJECT_NAME} ${module_name})
	durin_start("Module_${module_name}")

	set(_durin_module_cmake_file "${DURIN_PROJECT_INTERMEDIATE_BUILD_DIR}/${module_name}/${module_name}.module.cmake")
	include("${_durin_module_cmake_file}")

	list(APPEND module_private_dependencies ${module_optional_private_dependencies})
	list(APPEND module_public_dependencies ${module_optional_public_dependencies})

	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${module_config_file}")

	if(module_reflect_headers)
		add_custom_command(
			OUTPUT ${module_export_file}
			COMMAND ${DHT_MAIN} generate_module_export_file -m ${module_name} ${DURIN_DHT_CONTEXT_ARGS} ${DURIN_DHT_PROJECT_FILE_ARGS}
			DEPENDS ${module_reflect_headers} "${_durin_module_cmake_file}"
			COMMENT "[DHT] Generating export metadata for ${module_name}"
		)

		add_custom_command(
			OUTPUT ${module_generated_srcs}
			COMMAND ${DHT_MAIN} generate_reflection_files -m ${module_name} ${DURIN_DHT_CONTEXT_ARGS} ${DURIN_DHT_PROJECT_FILE_ARGS}
			DEPENDS ${module_reflect_headers} "${_durin_module_cmake_file}" ${module_manifest_dependencies} ${module_export_file}
			COMMENT "[DHT] Generating reflection files for ${module_name}"
		)
	endif()

	if("${module_link_type}" STREQUAL "STATIC")
		set(_durin_module_link_type STATIC)
	else()
		set(_durin_module_link_type SHARED)
	endif()

	add_library(${module_name} ${_durin_module_link_type})
	target_sources(${module_name} PUBLIC ${module_public_srcs} PRIVATE ${module_private_srcs} ${module_generated_srcs})

	set_target_properties(${module_name} PROPERTIES OUTPUT_NAME "${DURIN_PROFILE_NAME}-${module_name}")

	if("${_durin_module_link_type}" STREQUAL "SHARED")
		string(TOUPPER ${module_name} _durin_module_name_upper)
		set_target_properties(${module_name} PROPERTIES DEFINE_SYMBOL "${_durin_module_name_upper}_EXPORTS")
	endif()

	durin_target_apply_common_definitions(${module_name} ${module_name})

	target_include_directories(${module_name} PRIVATE
		${DURIN_PROJECT_INTERMEDIATE_BUILD_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/Private
	)

	target_include_directories(${module_name} PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/Public
		${module_dht_output_dir}
	)

	target_link_libraries(${module_name} PRIVATE ${module_private_dependencies})
	target_link_libraries(${module_name} PUBLIC ${module_public_dependencies})

	if(DURIN_ENABLE_PCH)
		if(module_pch_target)
			target_precompile_headers(${module_name} REUSE_FROM ${module_pch_target})
		else()
			target_precompile_headers(${module_name} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Private/PCH.${module_name}.h>")
		endif()
	elseif(module_pch_target)
		get_property(_durin_shared_pch_header GLOBAL PROPERTY "DURIN_SHARED_PCH_HEADER_${module_pch_target}")
		if(_durin_shared_pch_header)
			durin_target_force_include_pch(${module_name} "${_durin_shared_pch_header}")
		endif()
	else()
		durin_target_force_include_pch(${module_name} "${CMAKE_CURRENT_SOURCE_DIR}/Private/PCH.${module_name}.h")
	endif()

	durin_target_set_runtime_outputs(${module_name})
	set_target_properties(${module_name} PROPERTIES FOLDER "${DURIN_PROJECT_NAME}/${module_dir}")

	durin_end()
endfunction()

function(add_durin_test target_name)
	add_executable(${target_name} ${ARGN})

	durin_target_apply_common_definitions(${target_name} ${target_name})

	set(_durin_test_root_dir "${DURIN_PROJECT_TEST_OUTPUT_ROOT}/${target_name}")
	set(_durin_test_bin_dir "${DURIN_PROJECT_TEST_OUTPUT_ROOT}/Bin")
	set(_durin_test_data_dir "${_durin_test_root_dir}/Data")
	set(_durin_test_work_dir "${_durin_test_root_dir}/Work")

	target_compile_definitions(${target_name} PRIVATE
		DURIN_TEST_ROOT_DIR="${_durin_test_root_dir}"
		DURIN_TEST_BIN_DIR="${_durin_test_bin_dir}"
		DURIN_TEST_DATA_DIR="${_durin_test_data_dir}"
		DURIN_TEST_WORK_DIR="${_durin_test_work_dir}"
	)

	if(DURIN_ENABLE_PCH AND TARGET SharedPCH_Core)
		target_precompile_headers(${target_name} REUSE_FROM SharedPCH_Core)
	elseif(DURIN_FORCE_INCLUDE_PCH)
		get_property(_durin_shared_pch_header GLOBAL PROPERTY "DURIN_SHARED_PCH_HEADER_SharedPCH_Core")
		if(_durin_shared_pch_header)
			durin_target_force_include_pch(${target_name} "${_durin_shared_pch_header}")
		endif()
	endif()

	set_target_properties(${target_name} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${_durin_test_bin_dir}"
		LIBRARY_OUTPUT_DIRECTORY "${_durin_test_bin_dir}"
		ARCHIVE_OUTPUT_DIRECTORY "${DURIN_PROJECT_LIB_OUTPUT_ROOT}/${target_name}"
		PDB_OUTPUT_DIRECTORY "${DURIN_PROJECT_SYMBOL_OUTPUT_ROOT}/${target_name}"
		DURIN_TEST_ROOT_DIR "${_durin_test_root_dir}"
		DURIN_TEST_BIN_DIR "${_durin_test_bin_dir}"
		DURIN_TEST_DATA_DIR "${_durin_test_data_dir}"
		DURIN_TEST_WORK_DIR "${_durin_test_work_dir}"
	)

	add_custom_command(TARGET ${target_name} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory
		"${_durin_test_bin_dir}"
		"${_durin_test_data_dir}"
		"${_durin_test_work_dir}"
		COMMENT "Preparing test sandbox: ${target_name}"
		VERBATIM
	)

	set_target_properties(${target_name} PROPERTIES FOLDER "Tests/${target_name}")
endfunction()
