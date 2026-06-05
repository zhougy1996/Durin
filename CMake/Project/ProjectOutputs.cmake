# Target output layout and runtime file deployment helpers.

include_guard(GLOBAL)

function(durin_target_set_runtime_outputs target)
	set_target_properties(${target} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${DURIN_PROJECT_RUNTIME_OUTPUT_DIR}"
		LIBRARY_OUTPUT_DIRECTORY "${DURIN_PROJECT_RUNTIME_OUTPUT_DIR}"
		ARCHIVE_OUTPUT_DIRECTORY "${DURIN_PROJECT_LIB_OUTPUT_ROOT}/${target}"
		PDB_OUTPUT_DIRECTORY "${DURIN_PROJECT_SYMBOL_OUTPUT_ROOT}/${target}"
	)
endfunction()

function(durin_target_deploy_target_binary target dependent_target)
	get_filename_component(file_name "$<TARGET_FILE_NAME:${dependent_target}>" NAME)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:${dependent_target}>"
		"${DURIN_PROJECT_EXTERNAL_RUNTIME_DIR}/${file_name}"
		COMMENT "Deploying target binary: ${file_name}"
		VERBATIM
	)
endfunction()

function(durin_target_copy_binary_to_output_dir target dependent_target)
	get_filename_component(file_name "$<TARGET_FILE_NAME:${dependent_target}>" NAME)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:${dependent_target}>"
		"$<TARGET_FILE_DIR:${target}>/${file_name}"
		COMMENT "Deploying target binary to output dir: ${file_name}"
		VERBATIM
	)
endfunction()

function(durin_target_copy_files_to_output_dir target file_list)
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

function(durin_test_get_sandbox_dir target property_name out_var)
	get_target_property(sandbox_dir ${target} ${property_name})
	if(NOT sandbox_dir OR sandbox_dir STREQUAL "sandbox_dir-NOTFOUND")
		message(FATAL_ERROR "Target ${target} is missing ${property_name}. Use add_durin_test() for native test targets.")
	endif()

	set(${out_var} "${sandbox_dir}" PARENT_SCOPE)
endfunction()

function(durin_test_deploy_target_binary target dependent_target)
	durin_test_get_sandbox_dir(${target} DURIN_TEST_BIN_DIR test_bin_dir)
	get_filename_component(file_name "$<TARGET_FILE_NAME:${dependent_target}>" NAME)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:${dependent_target}>"
		"${test_bin_dir}/${file_name}"
		COMMENT "Deploying test binary dependency: ${file_name}"
		VERBATIM
	)
endfunction()

function(durin_test_deploy_files_to_bin target file_list)
	durin_test_get_sandbox_dir(${target} DURIN_TEST_BIN_DIR test_bin_dir)
	foreach(file_path ${file_list})
		get_filename_component(file_name "${file_path}" NAME)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${file_path}"
			"${test_bin_dir}/${file_name}"
			COMMENT "Deploying test runtime file: ${file_name}"
			VERBATIM
		)
	endforeach()
endfunction()

function(durin_test_deploy_runtime_files target dependent_target)
	get_target_property(runtime_files ${dependent_target} DURIN_RUNTIME_DEPLOY_FILES)
	if(NOT runtime_files OR runtime_files STREQUAL "runtime_files-NOTFOUND")
		return()
	endif()

	durin_test_deploy_files_to_bin(${target} "${runtime_files}")
endfunction()

function(durin_test_deploy_files_to_data target file_list)
	durin_test_get_sandbox_dir(${target} DURIN_TEST_DATA_DIR test_data_dir)
	foreach(file_path ${file_list})
		get_filename_component(file_name "${file_path}" NAME)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${file_path}"
			"${test_data_dir}/${file_name}"
			COMMENT "Deploying test data file: ${file_name}"
			VERBATIM
		)
	endforeach()
endfunction()

function(durin_test_deploy_directory_to_data target source_dir)
	durin_test_get_sandbox_dir(${target} DURIN_TEST_DATA_DIR test_data_dir)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${source_dir}"
		"${test_data_dir}"
		COMMENT "Deploying test data directory: ${source_dir}"
		VERBATIM
	)
endfunction()

function(durin_target_deploy_files target file_list)
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

function(durin_target_deploy_runtime_files target dependent_target)
	get_target_property(runtime_files ${dependent_target} DURIN_RUNTIME_DEPLOY_FILES)
	if(NOT runtime_files OR runtime_files STREQUAL "runtime_files-NOTFOUND")
		return()
	endif()

	durin_target_deploy_files(${target} "${runtime_files}")
endfunction()

function(durin_target_enable_delay_load target dll_name)
	if(NOT WIN32)
		return()
	endif()

	target_link_options(${target} PRIVATE "/DELAYLOAD:${dll_name}")
	target_link_libraries(${target} PRIVATE delayimp)
endfunction()
