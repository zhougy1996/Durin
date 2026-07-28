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

function(durin_target_enable_windows_long_paths target_name)
	if(NOT WIN32)
		return()
	endif()

	find_program(DURIN_MANIFEST_TOOL NAMES mt REQUIRED)
	set(_durin_long_path_manifest "${DURIN_WORKSPACE_DIR}/CMake/Windows/DurinLongPathAware.manifest")
	target_sources(${target_name} PRIVATE "${_durin_long_path_manifest}")
	add_custom_command(TARGET ${target_name} POST_BUILD
		COMMAND "${Python_EXECUTABLE}"
			"${DURIN_WORKSPACE_DIR}/Engine/Scripts/Build/verify_windows_manifest.py"
			--executable "$<TARGET_FILE:${target_name}>"
			--manifest-tool "${DURIN_MANIFEST_TOOL}"
		COMMENT "Verifying long-path-aware manifest: ${target_name}"
		VERBATIM
	)
endfunction()

# Rejects forbidden libraries anywhere in a target's concrete link dependency closure.
function(durin_assert_target_dependency_closure_excludes target_name)
	if(NOT TARGET ${target_name})
		message(FATAL_ERROR "Cannot inspect missing target ${target_name}.")
	endif()

	set(_durin_forbidden_dependencies ${ARGN})
	set(_durin_dependency_queue ${target_name})
	set(_durin_visited_dependencies)
	while(_durin_dependency_queue)
		list(POP_FRONT _durin_dependency_queue _durin_dependency)
		if(_durin_dependency IN_LIST _durin_visited_dependencies)
			continue()
		endif()
		list(APPEND _durin_visited_dependencies ${_durin_dependency})

		get_target_property(_durin_aliased_dependency ${_durin_dependency} ALIASED_TARGET)
		if(_durin_aliased_dependency)
			set(_durin_dependency ${_durin_aliased_dependency})
		endif()
		get_target_property(_durin_private_links ${_durin_dependency} LINK_LIBRARIES)
		get_target_property(_durin_public_links ${_durin_dependency} INTERFACE_LINK_LIBRARIES)
		foreach(_durin_link IN LISTS _durin_private_links _durin_public_links)
			if(_durin_link MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
				set(_durin_link "${CMAKE_MATCH_1}")
			elseif(_durin_link MATCHES "^\\$<TARGET_NAME_IF_EXISTS:([^>]+)>$")
				set(_durin_link "${CMAKE_MATCH_1}")
			elseif(_durin_link MATCHES "^\\$<")
				continue()
			endif()
			if(_durin_link IN_LIST _durin_forbidden_dependencies)
				message(FATAL_ERROR
					"Runtime target ${target_name} dependency closure includes forbidden dependency ${_durin_link}.")
			endif()
			if(TARGET ${_durin_link})
				list(APPEND _durin_dependency_queue ${_durin_link})
			endif()
		endforeach()
	endwhile()
endfunction()

function(add_durin_module module_name)
	durin_module_log(${DURIN_PROJECT_NAME} ${module_name})
	durin_start("Module_${module_name}")

	set(_durin_module_cmake_file "${DURIN_PROJECT_INTERMEDIATE_BUILD_DIR}/${module_name}/${module_name}.module.cmake")
	include("${_durin_module_cmake_file}")

	list(APPEND module_private_dependencies ${module_optional_private_dependencies})
	list(APPEND module_public_dependencies ${module_optional_public_dependencies})

	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${module_config_file}")

	# CMake owns ordinary source discovery so adding or removing a file updates
	# the generated build graph without requiring a manual configure step.
	file(GLOB_RECURSE module_public_srcs CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
		"${module_dir}/Public/*.cpp"
		"${module_dir}/Public/*.cc"
		"${module_dir}/Public/*.cxx"
		"${module_dir}/Public/*.h"
		"${module_dir}/Public/*.hpp"
		"${module_dir}/Public/*.inl"
	)
	file(GLOB_RECURSE module_private_srcs CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
		"${module_dir}/Private/*.cpp"
		"${module_dir}/Private/*.cc"
		"${module_dir}/Private/*.cxx"
		"${module_dir}/Private/*.h"
		"${module_dir}/Private/*.hpp"
		"${module_dir}/Private/*.inl"
	)

	if(module_reflect_headers)
		set(_durin_module_export_stamp "${module_dht_output_dir}/${module_name}.export.stamp")
		set(_durin_module_reflection_stamp "${module_dht_output_dir}/${module_name}.reflection.stamp")

		add_custom_command(
			OUTPUT "${_durin_module_export_stamp}"
			BYPRODUCTS "${module_export_file}" "${module_export_manifest_file}"
			COMMAND ${DHT_MAIN} generate_module_export_file -m ${module_name} --workers ${DURIN_DHT_WORKERS} --log ${DURIN_DHT_LOG_LEVEL} ${DURIN_DHT_CONTEXT_ARGS} ${DURIN_DHT_PROJECT_FILE_ARGS}
			COMMAND ${CMAKE_COMMAND} -E touch "${_durin_module_export_stamp}"
			DEPENDS ${module_reflect_headers} "${_durin_module_cmake_file}" "${DURIN_DHT_TOOL_FINGERPRINT_FILE}"
			COMMENT "[DHT] Generating export metadata for ${module_name}"
			JOB_POOL durin_dht
			VERBATIM
		)

		add_custom_command(
			OUTPUT "${_durin_module_reflection_stamp}"
			BYPRODUCTS ${module_generated_srcs} "${module_manifest_file}"
			COMMAND ${DHT_MAIN} generate_reflection_files -m ${module_name} --workers ${DURIN_DHT_WORKERS} --log ${DURIN_DHT_LOG_LEVEL} ${DURIN_DHT_CONTEXT_ARGS} ${DURIN_DHT_PROJECT_FILE_ARGS}
			COMMAND ${CMAKE_COMMAND} -E touch "${_durin_module_reflection_stamp}"
			DEPENDS ${module_reflect_headers} "${_durin_module_cmake_file}" "${DURIN_DHT_TOOL_FINGERPRINT_FILE}" ${module_reflection_export_dependencies} ${module_export_file}
			COMMENT "[DHT] Generating reflection files for ${module_name}"
			JOB_POOL durin_dht
			VERBATIM
		)

		# BYPRODUCTS describe ownership but the stamp itself must remain reachable
		# from the module target. This order-only target edge also guarantees all
		# generated sources exist before Ninja starts compiling the module.
		add_custom_target(${module_name}_DHT DEPENDS
			"${_durin_module_export_stamp}"
			"${module_export_file}"
			"${module_export_manifest_file}"
			"${_durin_module_reflection_stamp}"
			${module_generated_srcs}
			"${module_manifest_file}"
		)
	endif()

	if("${module_link_type}" STREQUAL "STATIC")
		set(_durin_module_link_type STATIC)
	else()
		set(_durin_module_link_type SHARED)
	endif()

	add_library(${module_name} ${_durin_module_link_type})
	if(TARGET ${module_name}_DHT)
		add_dependencies(${module_name} ${module_name}_DHT)
	endif()
	target_sources(${module_name} PUBLIC ${module_public_srcs} PRIVATE ${module_private_srcs} ${module_generated_srcs})

	set_target_properties(${module_name} PROPERTIES OUTPUT_NAME "${DURIN_RUNTIME_VARIANT}-${module_name}")

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
	if(NOT TARGET NativeTestSupport)
		message(FATAL_ERROR
			"NativeTestSupport must be defined before add_durin_test(${target_name}).")
	endif()

	set(_durin_test_root_dir "${DURIN_PROJECT_TEST_OUTPUT_ROOT}/${target_name}")
	set(_durin_test_bin_dir "${DURIN_PROJECT_TEST_OUTPUT_ROOT}/Bin")
	set(_durin_test_data_dir "${_durin_test_root_dir}/Data")
	set(_durin_test_work_dir "${_durin_test_root_dir}/Work")
	set(_durin_native_test_main
		"${CMAKE_CURRENT_BINARY_DIR}/Generated/$<CONFIG>/${target_name}NativeTestMain.cpp")
	set(DURIN_NATIVE_TEST_WORK_ROOT "${_durin_test_work_dir}")
	file(READ
		"${CMAKE_SOURCE_DIR}/Engine/Tests/NativeTestSupport/Private/NativeTestMain.cpp.in"
		_durin_native_test_main_template)
	string(CONFIGURE
		"${_durin_native_test_main_template}"
		_durin_native_test_main_content
		@ONLY)
	file(GENERATE
		OUTPUT "${_durin_native_test_main}"
		CONTENT "${_durin_native_test_main_content}"
	)

	add_executable(${target_name}
		${ARGN}
		"${_durin_native_test_main}"
	)
	durin_target_enable_windows_long_paths(${target_name})
	target_link_libraries(${target_name} PRIVATE NativeTestSupport)

	durin_target_apply_common_definitions(${target_name} ${target_name})

	# Keep native-test source ownership machine-checkable. Production sources
	# compiled directly into a test executable live outside this tree and are
	# intentionally ignored here.
	set(_durin_native_test_root
		"${CMAKE_SOURCE_DIR}/Engine/Tests/Native")
	foreach(_durin_test_source IN LISTS ARGN)
		if(IS_ABSOLUTE "${_durin_test_source}")
			cmake_path(NORMAL_PATH _durin_test_source
				OUTPUT_VARIABLE _durin_test_source_absolute)
		else()
			cmake_path(ABSOLUTE_PATH _durin_test_source
				BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
				NORMALIZE
				OUTPUT_VARIABLE _durin_test_source_absolute)
		endif()
		cmake_path(IS_PREFIX _durin_native_test_root
			"${_durin_test_source_absolute}"
			NORMALIZE
			_durin_is_native_test_source)
		if(_durin_is_native_test_source
			AND _durin_test_source_absolute MATCHES "\\.cpp$")
			get_property(_durin_existing_owner GLOBAL
				PROPERTY "DURIN_NATIVE_TEST_SOURCE_OWNER_${_durin_test_source_absolute}")
			if(_durin_existing_owner)
				message(FATAL_ERROR
					"Native test source ${_durin_test_source_absolute} is assigned "
					"to both ${_durin_existing_owner} and ${target_name}.")
			endif()
			set_property(GLOBAL
				PROPERTY "DURIN_NATIVE_TEST_SOURCE_OWNER_${_durin_test_source_absolute}"
				"${target_name}")
			set_property(GLOBAL APPEND
				PROPERTY DURIN_OWNED_NATIVE_TEST_SOURCES
				"${_durin_test_source_absolute}")
		endif()
	endforeach()

	target_compile_definitions(${target_name} PRIVATE
		DURIN_TEST_ROOT_DIR="${_durin_test_root_dir}"
		DURIN_TEST_BIN_DIR="${_durin_test_bin_dir}"
		DURIN_TEST_DATA_DIR="${_durin_test_data_dir}"
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
	set_property(GLOBAL APPEND PROPERTY DURIN_NATIVE_TEST_TARGETS "${target_name}")
endfunction()

function(durin_validate_native_test_source_ownership native_test_root)
	file(GLOB_RECURSE _durin_native_test_sources
		CONFIGURE_DEPENDS
		"${native_test_root}/*.cpp")
	get_property(_durin_owned_sources GLOBAL
		PROPERTY DURIN_OWNED_NATIVE_TEST_SOURCES)
	list(SORT _durin_native_test_sources)
	list(SORT _durin_owned_sources)

	set(_durin_unowned_sources ${_durin_native_test_sources})
	list(REMOVE_ITEM _durin_unowned_sources ${_durin_owned_sources})
	if(_durin_unowned_sources)
		list(JOIN _durin_unowned_sources "\n  " _durin_unowned_text)
		message(FATAL_ERROR
			"Native test sources are not assigned to an execution domain:\n"
			"  ${_durin_unowned_text}")
	endif()

	set(_durin_stale_sources ${_durin_owned_sources})
	list(REMOVE_ITEM _durin_stale_sources ${_durin_native_test_sources})
	if(_durin_stale_sources)
		list(JOIN _durin_stale_sources "\n  " _durin_stale_text)
		message(FATAL_ERROR
			"Native test ownership contains missing sources:\n"
			"  ${_durin_stale_text}")
	endif()

	list(LENGTH _durin_native_test_sources _durin_native_test_source_count)
	set_property(GLOBAL PROPERTY DURIN_NATIVE_TEST_SOURCE_COUNT
		"${_durin_native_test_source_count}")
	message(STATUS
		"Validated unique ownership for ${_durin_native_test_source_count} "
		"native test sources")

	set(_durin_forbidden_catch_all_targets
		CoreTests
		CoreDObjectTests
		AssetCoreTests
		RenderCoreTests
		VulkanRHITests
		EngineTests
	)
	get_property(_durin_native_test_targets GLOBAL
		PROPERTY DURIN_NATIVE_TEST_TARGETS)
	foreach(_durin_target IN LISTS _durin_native_test_targets)
		if(_durin_target IN_LIST _durin_forbidden_catch_all_targets)
			message(FATAL_ERROR
				"${_durin_target} mirrors a production module and is too broad "
				"to be a native-test execution domain.")
		endif()

		get_target_property(_durin_links ${_durin_target} LINK_LIBRARIES)
		set(_durin_heavy_links)
		foreach(_durin_link IN LISTS _durin_links)
			if(_durin_link IN_LIST DURIN_NATIVE_TEST_HEAVY_RUNTIME_LIBRARIES)
				list(APPEND _durin_heavy_links "${_durin_link}")
			endif()
		endforeach()
		if(_durin_heavy_links)
			get_target_property(_durin_runtime_rationale ${_durin_target}
				DURIN_TEST_HEAVY_RUNTIME_RATIONALE)
			if(_durin_runtime_rationale MATCHES "-NOTFOUND$"
				OR _durin_runtime_rationale STREQUAL "")
				list(JOIN _durin_heavy_links ", " _durin_heavy_text)
				message(FATAL_ERROR
					"${_durin_target} links heavyweight native-test runtime "
					"libraries (${_durin_heavy_text}) without "
					"DURIN_TEST_HEAVY_RUNTIME_RATIONALE.")
			endif()
		endif()
	endforeach()

	foreach(_durin_source IN LISTS _durin_native_test_sources)
		file(READ "${_durin_source}" _durin_source_content)
		string(REGEX MATCHALL
			"(TEST|TEST_F|TEST_P)[ \t\r\n]*\\([ \t\r\n]*[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*,"
			_durin_suite_declarations
			"${_durin_source_content}")
		get_property(_durin_source_owner GLOBAL
			PROPERTY "DURIN_NATIVE_TEST_SOURCE_OWNER_${_durin_source}")
		foreach(_durin_declaration IN LISTS _durin_suite_declarations)
			string(REGEX REPLACE
				"^(TEST|TEST_F|TEST_P)[ \t\r\n]*\\([ \t\r\n]*([A-Za-z_][A-Za-z0-9_]*)[ \t\r\n]*,$"
				"\\2"
				_durin_suite
				"${_durin_declaration}")
			get_property(_durin_suite_owner GLOBAL
				PROPERTY "DURIN_NATIVE_TEST_SUITE_OWNER_${_durin_suite}")
			if(_durin_suite_owner
				AND NOT _durin_suite_owner STREQUAL _durin_source_owner)
				message(FATAL_ERROR
					"GoogleTest suite ${_durin_suite} is registered by both "
					"${_durin_suite_owner} and ${_durin_source_owner}.")
			endif()
			set_property(GLOBAL
				PROPERTY "DURIN_NATIVE_TEST_SUITE_OWNER_${_durin_suite}"
				"${_durin_source_owner}")
		endforeach()
	endforeach()
endfunction()

function(durin_validate_native_test_repository_policy native_test_root)
	file(GLOB_RECURSE _durin_native_test_policy_files
		"${native_test_root}/*.cpp"
		"${native_test_root}/*.h")
	foreach(_durin_policy_file IN LISTS _durin_native_test_policy_files)
		file(READ "${_durin_policy_file}" _durin_policy_content)
		if(_durin_policy_content MATCHES "DURIN_TEST_WORK_DIR")
			message(FATAL_ERROR
				"${_durin_policy_file} uses the retired DURIN_TEST_WORK_DIR "
				"macro. Use Durin::Testing sandbox APIs.")
		endif()
		if(_durin_policy_content MATCHES
			"std::filesystem::remove_all[ \t\r\n]*\\(")
			message(FATAL_ERROR
				"${_durin_policy_file} calls std::filesystem::remove_all "
				"directly. Use Durin::Testing::RemoveTestWorkDirectory.")
		endif()
		if(_durin_policy_content MATCHES
			"(std::ofstream|std::fstream|std::filesystem::(create_directories|remove|rename))[^;]*DURIN_TEST_DATA_DIR"
			OR _durin_policy_content MATCHES
			"DURIN_TEST_DATA_DIR[^;]*(std::ofstream|std::fstream|std::filesystem::(create_directories|remove|rename))")
			message(FATAL_ERROR
				"${_durin_policy_file} appears to mutate checked-in test Data. "
				"Copy the input into the current process sandbox first.")
		endif()
	endforeach()

	file(GLOB_RECURSE _durin_native_test_cmake_files
		"${native_test_root}/CMakeLists.txt")
	foreach(_durin_cmake_file IN LISTS _durin_native_test_cmake_files)
		file(READ "${_durin_cmake_file}" _durin_cmake_content)
		if(_durin_cmake_content MATCHES
			"(^|[^A-Za-z0-9_])gtest_discover_tests[ \t\r\n]*\\(")
			message(FATAL_ERROR
				"${_durin_cmake_file} registers GoogleTest cases directly. "
				"Use durin_discover_tests so isolation and resource policy apply.")
		endif()
	endforeach()
endfunction()

set(DURIN_NATIVE_TEST_RESOURCE_LOCK_REGISTRY
	durin-gpu
)
set(DURIN_NATIVE_TEST_LEGACY_RESOURCE_GROUP_REGISTRY
	renderer-runtime
)
set(DURIN_NATIVE_TEST_HEAVY_RUNTIME_LIBRARIES
	Renderer
	VulkanRHI
	DurinEd
	Mona
	MonaImGui
)

function(durin_resolve_native_test_discovery_policy
	out_resource_locks
	out_labels
	target_name
	case_parallel_safe
	legacy_serialization_group
)
	set(options)
	set(one_value_args TARGET_LOCK_RATIONALE)
	set(multi_value_args RESOURCE_LOCKS LABELS)
	cmake_parse_arguments(
		DURIN_TEST_POLICY
		"${options}"
		"${one_value_args}"
		"${multi_value_args}"
		${ARGN}
	)
	if(DURIN_TEST_POLICY_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR
			"Unknown native-test discovery policy arguments: ${DURIN_TEST_POLICY_UNPARSED_ARGUMENTS}")
	endif()

	if(NOT case_parallel_safe STREQUAL "TRUE" AND NOT case_parallel_safe STREQUAL "FALSE")
		message(FATAL_ERROR
			"${target_name} DURIN_TEST_CASE_PARALLEL_SAFE must resolve to TRUE or FALSE.")
	endif()

	set(_durin_resource_locks ${DURIN_TEST_POLICY_RESOURCE_LOCKS})
	foreach(_durin_resource_lock IN LISTS _durin_resource_locks)
		if(NOT _durin_resource_lock IN_LIST DURIN_NATIVE_TEST_RESOURCE_LOCK_REGISTRY)
			message(FATAL_ERROR
				"${target_name} requests unregistered native-test resource "
				"lock '${_durin_resource_lock}'.")
		endif()
	endforeach()
	if(case_parallel_safe
		AND "durin-test-target-${target_name}" IN_LIST _durin_resource_locks)
		message(FATAL_ERROR
			"${target_name} cannot be case-parallel and explicitly target-serialized.")
	endif()
	if(NOT case_parallel_safe)
		if(NOT DURIN_TEST_POLICY_TARGET_LOCK_RATIONALE)
			message(FATAL_ERROR
				"${target_name} requires DURIN_TEST_TARGET_LOCK_RATIONALE "
				"before broad target serialization can be enabled.")
		endif()
		list(APPEND _durin_resource_locks "durin-test-target-${target_name}")
	endif()
	if(legacy_serialization_group)
		if(NOT legacy_serialization_group MATCHES "^[A-Za-z0-9_.-]+$")
			message(FATAL_ERROR
				"${target_name} has invalid DURIN_TEST_LEGACY_SERIALIZATION_GROUP "
				"'${legacy_serialization_group}'.")
		endif()
		if(NOT legacy_serialization_group IN_LIST
			DURIN_NATIVE_TEST_LEGACY_RESOURCE_GROUP_REGISTRY)
			message(FATAL_ERROR
				"${target_name} requests unregistered legacy serialization "
				"group '${legacy_serialization_group}'.")
		endif()
		list(APPEND _durin_resource_locks
			"durin-test-legacy-${legacy_serialization_group}")
	endif()
	list(REMOVE_DUPLICATES _durin_resource_locks)

	set(_durin_labels native-test "${target_name}" ${DURIN_TEST_POLICY_LABELS})
	list(REMOVE_DUPLICATES _durin_labels)

	set(${out_resource_locks} "${_durin_resource_locks}" PARENT_SCOPE)
	set(${out_labels} "${_durin_labels}" PARENT_SCOPE)
endfunction()

function(durin_discover_tests target_name)
	if(NOT TARGET ${target_name})
		message(FATAL_ERROR "Cannot discover tests for missing target ${target_name}.")
	endif()

	get_target_property(_durin_work_dir ${target_name} DURIN_TEST_WORK_DIR)
	if(NOT _durin_work_dir)
		message(FATAL_ERROR
			"${target_name} must be created with add_durin_test before discovery.")
	endif()

	get_target_property(_durin_case_parallel_safe
		${target_name} DURIN_TEST_CASE_PARALLEL_SAFE)
	if(_durin_case_parallel_safe MATCHES "-NOTFOUND$")
		set(_durin_case_parallel_safe FALSE)
	elseif(NOT _durin_case_parallel_safe STREQUAL "TRUE"
		AND NOT _durin_case_parallel_safe STREQUAL "FALSE")
		message(FATAL_ERROR
			"${target_name} DURIN_TEST_CASE_PARALLEL_SAFE must be TRUE or FALSE.")
	endif()

	get_target_property(_durin_legacy_group
		${target_name} DURIN_TEST_LEGACY_SERIALIZATION_GROUP)
	if(_durin_legacy_group MATCHES "-NOTFOUND$")
		set(_durin_legacy_group)
	endif()
	get_target_property(_durin_explicit_locks
		${target_name} DURIN_TEST_RESOURCE_LOCKS)
	if(_durin_explicit_locks MATCHES "-NOTFOUND$")
		set(_durin_explicit_locks)
	endif()
	get_target_property(_durin_extra_labels ${target_name} DURIN_TEST_LABELS)
	if(_durin_extra_labels MATCHES "-NOTFOUND$")
		set(_durin_extra_labels)
	endif()
	get_target_property(_durin_timeout ${target_name} DURIN_TEST_TIMEOUT)
	if(_durin_timeout MATCHES "-NOTFOUND$")
		set(_durin_timeout 300)
	endif()
	get_target_property(_durin_target_lock_rationale ${target_name}
		DURIN_TEST_TARGET_LOCK_RATIONALE)
	if(_durin_target_lock_rationale MATCHES "-NOTFOUND$")
		set(_durin_target_lock_rationale)
	endif()
	if(NOT _durin_timeout MATCHES "^[1-9][0-9]*$")
		message(FATAL_ERROR
			"${target_name} DURIN_TEST_TIMEOUT must be a positive integer.")
	endif()

	durin_resolve_native_test_discovery_policy(
		_durin_resource_locks
		_durin_labels
		"${target_name}"
		"${_durin_case_parallel_safe}"
		"${_durin_legacy_group}"
		RESOURCE_LOCKS ${_durin_explicit_locks}
		LABELS ${_durin_extra_labels}
		TARGET_LOCK_RATIONALE "${_durin_target_lock_rationale}"
	)

	set_target_properties(${target_name} PROPERTIES
		DURIN_TEST_DISCOVERY_RESOURCE_LOCKS "${_durin_resource_locks}"
		DURIN_TEST_DISCOVERY_LABELS "${_durin_labels}"
	)

	# gtest_discover_tests forwards PROPERTIES through a generated -D argument
	# and a second CMake script. Preserve list-valued property arguments across
	# both list expansions.
	string(REPLACE ";" "\\\\;" _durin_labels_property "${_durin_labels}")
	string(REPLACE ";" "\\\\;" _durin_locks_property "${_durin_resource_locks}")
	set(_durin_test_properties
		TIMEOUT "${_durin_timeout}"
		LABELS "${_durin_labels_property}"
	)
	if(_durin_resource_locks)
		list(APPEND _durin_test_properties
			RESOURCE_LOCK "${_durin_locks_property}")
	endif()

	gtest_discover_tests(${target_name}
		WORKING_DIRECTORY "${_durin_work_dir}"
		DISCOVERY_TIMEOUT 30
		PROPERTIES ${_durin_test_properties}
	)

	add_test(
		NAME "Durin.NativeTestDirect.${target_name}"
		COMMAND "$<TARGET_FILE:${target_name}>" --gtest_brief=1
		WORKING_DIRECTORY "${_durin_work_dir}"
	)
	set(_durin_direct_labels
		${_durin_labels}
		native-test-direct
	)
	set_tests_properties(
		"Durin.NativeTestDirect.${target_name}"
		PROPERTIES
			TIMEOUT "${_durin_timeout}"
			LABELS "${_durin_direct_labels}"
	)
	if(_durin_resource_locks)
		set_tests_properties(
			"Durin.NativeTestDirect.${target_name}"
			PROPERTIES
				RESOURCE_LOCK "${_durin_resource_locks}"
		)
	endif()
endfunction()
