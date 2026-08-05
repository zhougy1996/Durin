# Target output layout and runtime file deployment helpers.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/TargetDependencyClosure.cmake")

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

function(durin_test_register_runtime_only_dependencies target)
	set(options)
	set(one_value_args RATIONALE)
	set(multi_value_args TARGETS FILES)
	cmake_parse_arguments(
		DURIN_RUNTIME_ONLY
		"${options}"
		"${one_value_args}"
		"${multi_value_args}"
		${ARGN}
	)
	if(DURIN_RUNTIME_ONLY_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR
			"Unknown runtime-only dependency arguments for ${target}: "
			"${DURIN_RUNTIME_ONLY_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT TARGET "${target}")
		message(FATAL_ERROR
			"Cannot register runtime-only dependencies for missing target ${target}.")
	endif()
	if(NOT DURIN_RUNTIME_ONLY_TARGETS AND NOT DURIN_RUNTIME_ONLY_FILES)
		message(FATAL_ERROR
			"${target} runtime-only dependency registration requires TARGETS or FILES.")
	endif()
	if(NOT DURIN_RUNTIME_ONLY_RATIONALE)
		message(FATAL_ERROR
			"${target} runtime-only dependencies require a non-empty RATIONALE.")
	endif()

	set(_durin_runtime_only_targets)
	foreach(_durin_runtime_only_target IN LISTS DURIN_RUNTIME_ONLY_TARGETS)
		if(NOT TARGET "${_durin_runtime_only_target}")
			message(FATAL_ERROR
				"${target} names missing runtime-only target "
				"${_durin_runtime_only_target}.")
		endif()
		durin_normalize_target_alias(
			_durin_runtime_only_target
			"${_durin_runtime_only_target}"
		)
		list(APPEND _durin_runtime_only_targets
			"${_durin_runtime_only_target}")
	endforeach()

	set(_durin_runtime_only_files)
	foreach(_durin_runtime_only_file IN LISTS DURIN_RUNTIME_ONLY_FILES)
		if(_durin_runtime_only_file MATCHES "\\$<")
			message(FATAL_ERROR
				"${target} runtime-only file '${_durin_runtime_only_file}' uses "
				"an unsupported generator expression.")
		endif()
		get_filename_component(_durin_runtime_only_file
			"${_durin_runtime_only_file}" ABSOLUTE)
		cmake_path(NORMAL_PATH _durin_runtime_only_file)
		list(APPEND _durin_runtime_only_files "${_durin_runtime_only_file}")
	endforeach()

	set_property(TARGET "${target}" APPEND PROPERTY
		DURIN_TEST_RUNTIME_ONLY_TARGETS ${_durin_runtime_only_targets})
	set_property(TARGET "${target}" APPEND PROPERTY
		DURIN_TEST_RUNTIME_ONLY_FILES ${_durin_runtime_only_files})
	set_property(TARGET "${target}" APPEND PROPERTY
		DURIN_TEST_RUNTIME_ONLY_RATIONALE "${DURIN_RUNTIME_ONLY_RATIONALE}")
endfunction()

function(durin_test_collect_runtime_dependency_closure
	target
	out_targets
	out_files
	out_visited
	out_linked
)
	if(NOT TARGET "${target}")
		message(FATAL_ERROR
			"Cannot collect native-test runtime closure for missing target ${target}.")
	endif()

	durin_collect_target_dependency_closure(_durin_linked_targets "${target}")
	get_target_property(_durin_runtime_only_targets
		"${target}" DURIN_TEST_RUNTIME_ONLY_TARGETS)
	if(_durin_runtime_only_targets MATCHES "-NOTFOUND$")
		set(_durin_runtime_only_targets)
	endif()
	foreach(_durin_runtime_only_target IN LISTS _durin_runtime_only_targets)
		if(NOT TARGET "${_durin_runtime_only_target}")
			message(FATAL_ERROR
				"${target} names missing runtime-only target "
				"${_durin_runtime_only_target}.")
		endif()
		durin_normalize_target_alias(
			_durin_runtime_only_target
			"${_durin_runtime_only_target}"
		)
		if(_durin_runtime_only_target IN_LIST _durin_linked_targets)
			message(FATAL_ERROR
				"${target} lists linked target ${_durin_runtime_only_target} as "
				"runtime-only. Remove the exception and use the ordinary link closure.")
		endif()
	endforeach()

	set(_durin_closure_roots "${target}" ${_durin_runtime_only_targets})
	durin_collect_target_dependency_closure(
		_durin_visited_targets
		${_durin_closure_roots}
	)

	set(_durin_deploy_targets)
	set(_durin_deploy_files)
	foreach(_durin_dependency IN LISTS _durin_visited_targets)
		get_target_property(_durin_runtime_files
			"${_durin_dependency}" DURIN_RUNTIME_DEPLOY_FILES)
		if(NOT _durin_runtime_files MATCHES "-NOTFOUND$")
			foreach(_durin_runtime_file IN LISTS _durin_runtime_files)
				if(_durin_runtime_file MATCHES "\\$<")
					message(FATAL_ERROR
						"Target ${_durin_dependency} publishes unsupported runtime "
						"file expression '${_durin_runtime_file}'.")
				endif()
				get_filename_component(_durin_runtime_file
					"${_durin_runtime_file}" ABSOLUTE)
				cmake_path(NORMAL_PATH _durin_runtime_file)
				list(APPEND _durin_deploy_files "${_durin_runtime_file}")
			endforeach()
		endif()

		get_target_property(_durin_dependency_type
			"${_durin_dependency}" TYPE)
		get_target_property(_durin_dependency_imported
			"${_durin_dependency}" IMPORTED)
		if(NOT "${_durin_dependency}" STREQUAL "${target}"
			AND NOT _durin_dependency_imported
			AND (_durin_dependency_type STREQUAL "SHARED_LIBRARY"
				OR _durin_dependency_type STREQUAL "MODULE_LIBRARY"))
			list(APPEND _durin_deploy_targets "${_durin_dependency}")
		endif()
	endforeach()

	get_target_property(_durin_runtime_only_files
		"${target}" DURIN_TEST_RUNTIME_ONLY_FILES)
	if(NOT _durin_runtime_only_files MATCHES "-NOTFOUND$")
		list(APPEND _durin_deploy_files ${_durin_runtime_only_files})
	endif()

	list(REMOVE_DUPLICATES _durin_deploy_targets)
	list(SORT _durin_deploy_targets)
	list(REMOVE_DUPLICATES _durin_deploy_files)
	list(SORT _durin_deploy_files)
	set(${out_targets} "${_durin_deploy_targets}" PARENT_SCOPE)
	set(${out_files} "${_durin_deploy_files}" PARENT_SCOPE)
	set(${out_visited} "${_durin_visited_targets}" PARENT_SCOPE)
	set(${out_linked} "${_durin_linked_targets}" PARENT_SCOPE)
endfunction()

function(durin_test_audit_runtime_dependency_closure target)
	durin_test_collect_runtime_dependency_closure(
		"${target}"
		_durin_derived_targets
		_durin_derived_files
		_durin_visited_targets
		_durin_linked_targets
	)

	get_target_property(_durin_manual_targets
		"${target}" DURIN_TEST_MANUAL_RUNTIME_TARGETS)
	if(_durin_manual_targets MATCHES "-NOTFOUND$")
		set(_durin_manual_targets)
	endif()
	get_target_property(_durin_manual_files
		"${target}" DURIN_TEST_MANUAL_RUNTIME_FILES)
	if(_durin_manual_files MATCHES "-NOTFOUND$")
		set(_durin_manual_files)
	endif()
	list(REMOVE_DUPLICATES _durin_manual_targets)
	list(SORT _durin_manual_targets)
	list(REMOVE_DUPLICATES _durin_manual_files)
	list(SORT _durin_manual_files)

	set(_durin_missing_targets ${_durin_derived_targets})
	list(REMOVE_ITEM _durin_missing_targets ${_durin_manual_targets})
	set(_durin_redundant_targets ${_durin_manual_targets})
	list(FILTER _durin_redundant_targets INCLUDE REGEX ".+")
	foreach(_durin_manual_target IN LISTS _durin_redundant_targets)
		if(NOT _durin_manual_target IN_LIST _durin_derived_targets)
			list(REMOVE_ITEM _durin_redundant_targets "${_durin_manual_target}")
		endif()
	endforeach()
	set(_durin_extra_targets ${_durin_manual_targets})
	list(REMOVE_ITEM _durin_extra_targets ${_durin_derived_targets})

	set(_durin_missing_files ${_durin_derived_files})
	list(REMOVE_ITEM _durin_missing_files ${_durin_manual_files})
	set(_durin_extra_files ${_durin_manual_files})
	list(REMOVE_ITEM _durin_extra_files ${_durin_derived_files})

	set_target_properties("${target}" PROPERTIES
		DURIN_TEST_DERIVED_RUNTIME_TARGETS "${_durin_derived_targets}"
		DURIN_TEST_DERIVED_RUNTIME_FILES "${_durin_derived_files}"
		DURIN_TEST_RUNTIME_VISITED_TARGETS "${_durin_visited_targets}"
		DURIN_TEST_LINKED_TARGETS "${_durin_linked_targets}"
	)

	if(DEFINED ENV{DURIN_NATIVE_TEST_RUNTIME_CLOSURE_AUDIT})
		message(STATUS "[NativeTestRuntimeClosure] ${target}")
		message(STATUS "  derived-targets=${_durin_derived_targets}")
		message(STATUS "  derived-files=${_durin_derived_files}")
		message(STATUS "  manual-targets=${_durin_manual_targets}")
		message(STATUS "  manual-files=${_durin_manual_files}")
		message(STATUS "  missing-targets=${_durin_missing_targets}")
		message(STATUS "  missing-files=${_durin_missing_files}")
		message(STATUS "  redundant-targets=${_durin_redundant_targets}")
		message(STATUS "  extra-targets=${_durin_extra_targets}")
		message(STATUS "  extra-files=${_durin_extra_files}")
	endif()
endfunction()

function(durin_test_register_runtime_dependency_closure target)
	durin_test_audit_runtime_dependency_closure("${target}")

	get_target_property(_durin_derived_targets
		"${target}" DURIN_TEST_DERIVED_RUNTIME_TARGETS)
	get_target_property(_durin_derived_files
		"${target}" DURIN_TEST_DERIVED_RUNTIME_FILES)
	get_target_property(_durin_manual_targets
		"${target}" DURIN_TEST_MANUAL_RUNTIME_TARGETS)
	if(_durin_manual_targets MATCHES "-NOTFOUND$")
		set(_durin_manual_targets)
	endif()
	get_target_property(_durin_manual_files
		"${target}" DURIN_TEST_MANUAL_RUNTIME_FILES)
	if(_durin_manual_files MATCHES "-NOTFOUND$")
		set(_durin_manual_files)
	endif()

	set(_durin_duplicate_targets ${_durin_manual_targets})
	foreach(_durin_manual_target IN LISTS _durin_duplicate_targets)
		if(NOT _durin_manual_target IN_LIST _durin_derived_targets)
			list(REMOVE_ITEM _durin_duplicate_targets "${_durin_manual_target}")
		endif()
	endforeach()
	if(_durin_duplicate_targets)
		list(JOIN _durin_duplicate_targets ", " _durin_duplicate_target_text)
		message(FATAL_ERROR
			"${target} manually deploys targets already provided by its derived "
			"runtime closure: ${_durin_duplicate_target_text}. Remove the manual "
			"declarations.")
	endif()

	set(_durin_duplicate_files ${_durin_manual_files})
	foreach(_durin_manual_file IN LISTS _durin_duplicate_files)
		if(NOT _durin_manual_file IN_LIST _durin_derived_files)
			list(REMOVE_ITEM _durin_duplicate_files "${_durin_manual_file}")
		endif()
	endforeach()
	if(_durin_duplicate_files)
		list(JOIN _durin_duplicate_files ", " _durin_duplicate_file_text)
		message(FATAL_ERROR
			"${target} manually deploys files already provided by its derived "
			"runtime closure: ${_durin_duplicate_file_text}. Remove the manual "
			"declarations.")
	endif()

	durin_test_get_sandbox_dir("${target}" DURIN_TEST_BIN_DIR _durin_test_bin_dir)
	foreach(_durin_deploy_target IN LISTS _durin_derived_targets)
		durin_test_get_target_binary_deployment(
			"${_durin_deploy_target}"
			"${_durin_test_bin_dir}"
			_durin_deployment_target
		)
		add_dependencies("${target}" "${_durin_deployment_target}")
	endforeach()
	foreach(_durin_deploy_file IN LISTS _durin_derived_files)
		durin_test_get_runtime_file_deployment(
			"${_durin_deploy_file}"
			"${_durin_test_bin_dir}"
			_durin_deployment_target
		)
		add_dependencies("${target}" "${_durin_deployment_target}")
	endforeach()
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
		set(deployment_stamp
			"${CMAKE_CURRENT_BINARY_DIR}/${deployment_target}.stamp")
		add_custom_command(
			OUTPUT "${deployment_stamp}"
			COMMAND ${CMAKE_COMMAND} -E make_directory
			"${test_bin_dir}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:${deployment_source_target}>"
			"${test_bin_dir}/$<TARGET_FILE_NAME:${deployment_source_target}>"
			COMMAND ${CMAKE_COMMAND} -E touch
			"${deployment_stamp}"
			DEPENDS ${deployment_source_target}
			COMMENT "Deploying shared test binary target: ${deployment_source_target}"
			VERBATIM
		)
		add_custom_target(${deployment_target}
			DEPENDS "${deployment_stamp}"
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
		add_custom_command(
			OUTPUT "${destination_path}"
			COMMAND ${CMAKE_COMMAND} -E make_directory
			"${test_bin_dir}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${source_path}"
			"${destination_path}"
			DEPENDS "${source_path}"
			COMMENT "Deploying shared test runtime file: ${file_name}"
			VERBATIM
		)
		add_custom_target(${deployment_target}
			DEPENDS "${destination_path}"
		)
		set_target_properties(${deployment_target} PROPERTIES
			FOLDER "Tests/Infrastructure/Runtime"
		)
	endif()

	set(${out_var} "${deployment_target}" PARENT_SCOPE)
endfunction()

function(durin_test_deploy_target_binary target dependent_target)
	durin_normalize_target_alias(_durin_dependent_target "${dependent_target}")
	set_property(TARGET "${target}" APPEND PROPERTY
		DURIN_TEST_MANUAL_RUNTIME_TARGETS "${_durin_dependent_target}")
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
		get_filename_component(_durin_runtime_file "${file_path}" ABSOLUTE)
		cmake_path(NORMAL_PATH _durin_runtime_file)
		set_property(TARGET "${target}" APPEND PROPERTY
			DURIN_TEST_MANUAL_RUNTIME_FILES "${_durin_runtime_file}")
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
