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

function(durin_test_get_target_binary_deployment dependent_target test_bin_dir out_var)
	if(NOT TARGET ${dependent_target})
		message(FATAL_ERROR
			"Cannot deploy missing native-test dependency target ${dependent_target}.")
	endif()

	get_target_property(aliased_target ${dependent_target} ALIASED_TARGET)
	if(aliased_target)
		set(deployment_source_target "${aliased_target}")
	else()
		set(deployment_source_target "${dependent_target}")
	endif()

	string(SHA256 deployment_hash
		"${test_bin_dir}|target|${deployment_source_target}")
	string(SUBSTRING "${deployment_hash}" 0 12 deployment_hash)
	string(MAKE_C_IDENTIFIER "${deployment_source_target}" deployment_label)
	set(deployment_target
		"DurinTestDeployTarget_${deployment_label}_${deployment_hash}")

	if(NOT TARGET ${deployment_target})
		add_custom_target(${deployment_target}
			COMMAND ${CMAKE_COMMAND} -E make_directory
			"${test_bin_dir}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:${deployment_source_target}>"
			"${test_bin_dir}/$<TARGET_FILE_NAME:${deployment_source_target}>"
			COMMENT "Deploying shared test binary target: ${deployment_source_target}"
			VERBATIM
		)
		set_target_properties(${deployment_target} PROPERTIES
			FOLDER "Tests/Infrastructure/Runtime"
		)
	endif()

	set(${out_var} "${deployment_target}" PARENT_SCOPE)
endfunction()

function(durin_test_get_runtime_file_deployment file_path test_bin_dir out_var)
	get_filename_component(source_path "${file_path}" ABSOLUTE)
	cmake_path(NORMAL_PATH source_path)
	get_filename_component(file_name "${source_path}" NAME)
	set(destination_path "${test_bin_dir}/${file_name}")

	set(destination_identity "${destination_path}")
	if(WIN32)
		string(TOLOWER "${destination_identity}" destination_identity)
	endif()
	string(SHA256 destination_hash "${destination_identity}")
	set(source_property
		"DURIN_TEST_RUNTIME_DEPLOYMENT_SOURCE_${destination_hash}")
	get_property(source_registered GLOBAL PROPERTY "${source_property}" SET)
	if(source_registered)
		get_property(existing_source GLOBAL PROPERTY "${source_property}")
		set(existing_source_identity "${existing_source}")
		set(source_identity "${source_path}")
		if(WIN32)
			string(TOLOWER "${existing_source_identity}" existing_source_identity)
			string(TOLOWER "${source_identity}" source_identity)
		endif()
		if(NOT existing_source_identity STREQUAL source_identity)
			message(FATAL_ERROR
				"Native-test runtime deployment collision for ${destination_path}:\n"
				"  ${existing_source}\n"
				"  ${source_path}")
		endif()
	else()
		set_property(GLOBAL PROPERTY "${source_property}" "${source_path}")
	endif()

	string(SUBSTRING "${destination_hash}" 0 12 deployment_hash)
	string(MAKE_C_IDENTIFIER "${file_name}" deployment_label)
	set(deployment_target
		"DurinTestDeployFile_${deployment_label}_${deployment_hash}")

	if(NOT TARGET ${deployment_target})
		get_filename_component(test_bin_dir "${destination_path}" DIRECTORY)
		add_custom_target(${deployment_target}
			COMMAND ${CMAKE_COMMAND} -E make_directory
			"${test_bin_dir}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${source_path}"
			"${destination_path}"
			DEPENDS "${source_path}"
			COMMENT "Deploying shared test runtime file: ${file_name}"
			VERBATIM
		)
		set_target_properties(${deployment_target} PROPERTIES
			FOLDER "Tests/Infrastructure/Runtime"
		)
	endif()

	set(${out_var} "${deployment_target}" PARENT_SCOPE)
endfunction()

function(durin_test_deploy_target_binary target dependent_target)
	durin_test_get_sandbox_dir(${target} DURIN_TEST_BIN_DIR test_bin_dir)
	durin_test_get_target_binary_deployment(
		${dependent_target}
		"${test_bin_dir}"
		deployment_target
	)
	add_dependencies(${target} ${deployment_target})
endfunction()

function(durin_test_deploy_files_to_bin target file_list)
	durin_test_get_sandbox_dir(${target} DURIN_TEST_BIN_DIR test_bin_dir)
	foreach(file_path ${file_list})
		durin_test_get_runtime_file_deployment(
			"${file_path}"
			"${test_bin_dir}"
			deployment_target
		)
		add_dependencies(${target} ${deployment_target})
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
