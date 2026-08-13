# Project target creation helpers for modules and native tests.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/TargetDependencyClosure.cmake")

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

# Requires named libraries in a target's concrete link dependency closure.
function(durin_assert_target_dependency_closure_includes target_name)
	set(_durin_required_dependencies ${ARGN})
	durin_collect_target_dependency_closure(
		_durin_dependency_closure
		"${target_name}"
	)

	foreach(_durin_required_dependency IN LISTS _durin_required_dependencies)
		if(TARGET "${_durin_required_dependency}")
			durin_normalize_target_alias(
				_durin_required_dependency
				"${_durin_required_dependency}"
			)
		endif()
		if(NOT _durin_required_dependency IN_LIST _durin_dependency_closure)
			message(FATAL_ERROR
				"Target ${target_name} dependency closure omits required dependency "
				"${_durin_required_dependency}.")
		endif()
	endforeach()
endfunction()

# Rejects forbidden libraries anywhere in a target's concrete link dependency closure.
function(durin_assert_target_dependency_closure_excludes target_name)
	set(_durin_forbidden_dependencies ${ARGN})
	durin_collect_target_dependency_closure(
		_durin_dependency_closure
		"${target_name}"
	)

	foreach(_durin_forbidden_dependency IN LISTS _durin_forbidden_dependencies)
		if(TARGET "${_durin_forbidden_dependency}")
			durin_normalize_target_alias(
				_durin_forbidden_dependency
				"${_durin_forbidden_dependency}"
			)
		endif()
		if(_durin_forbidden_dependency IN_LIST _durin_dependency_closure)
			message(FATAL_ERROR
				"Target ${target_name} dependency closure includes "
				"forbidden dependency ${_durin_forbidden_dependency}.")
		endif()
	endforeach()
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
			BYPRODUCTS "${module_export_file}"
			COMMAND ${DHT_MAIN} generate_module_export_file -m ${module_name} --workers ${DURIN_DHT_WORKERS} --log ${DURIN_DHT_LOG_LEVEL} ${DURIN_DHT_CONTEXT_ARGS} ${DURIN_DHT_PROJECT_FILE_ARGS}
			COMMAND ${CMAKE_COMMAND} -E touch "${_durin_module_export_stamp}"
			DEPENDS ${module_reflect_headers} "${_durin_module_cmake_file}" "${DURIN_DHT_TOOL_FINGERPRINT_FILE}" ${module_export_dependencies}
			COMMENT "[DHT] Generating export metadata for ${module_name}"
			JOB_POOL durin_dht
			VERBATIM
		)

		add_custom_command(
			OUTPUT "${_durin_module_reflection_stamp}"
			BYPRODUCTS ${module_generated_srcs}
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
			"${_durin_module_reflection_stamp}"
			${module_generated_srcs}
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
	# Generated sources share the current binary directory, so retain a stable
	# per-target suffix without repeating long target names in object paths.
	string(SHA256 _durin_native_test_main_hash "${target_name}")
	string(SUBSTRING "${_durin_native_test_main_hash}" 0 12
		_durin_native_test_main_suffix)
	set(_durin_native_test_main
		"${CMAKE_CURRENT_BINARY_DIR}/Generated/$<CONFIG>/NativeTestMain-${_durin_native_test_main_suffix}.cpp")
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
	if(MSVC)
		# Native-test executables are runtime artifacts; keeping one incremental
		# linker database per target makes the shared test Bin grow rapidly.
		target_link_options(${target_name} PRIVATE "/INCREMENTAL:NO")
	endif()
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
		EXCLUDE_FROM_ALL TRUE
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

set(DURIN_NATIVE_TEST_KINDS
	contract
	feature
	integration
	characterization
	infrastructure
	qualification
)
set(DURIN_NATIVE_TEST_RESERVED_LABEL_PREFIXES
	kind-
	domain-
	module-
	backend-
	stack-
)

function(durin_validate_native_test_metadata_value out_value dimension value)
	string(TOLOWER "${value}" _durin_value)
	if(NOT _durin_value MATCHES "^[a-z][a-z0-9]*(-[a-z0-9]+)*$")
		message(FATAL_ERROR
			"Native-test ${dimension} value '${value}' is invalid; use a lowercase "
			"slug matching [a-z][a-z0-9]*(-[a-z0-9]+)*.")
	endif()
	set(${out_value} "${_durin_value}" PARENT_SCOPE)
endfunction()

function(durin_normalize_native_test_metadata_list out_values dimension)
	set(_durin_normalized)
	foreach(_durin_value IN LISTS ARGN)
		durin_validate_native_test_metadata_value(
			_durin_normalized_value "${dimension}" "${_durin_value}")
		if(_durin_normalized_value IN_LIST _durin_normalized)
			message(FATAL_ERROR
				"Native-test ${dimension} metadata contains duplicate value "
				"'${_durin_normalized_value}'.")
		endif()
		list(APPEND _durin_normalized "${_durin_normalized_value}")
	endforeach()
	list(SORT _durin_normalized)
	set(${out_values} "${_durin_normalized}" PARENT_SCOPE)
endfunction()

function(durin_assert_native_test_labels_not_reserved target_name)
	foreach(_durin_label IN LISTS ARGN)
		foreach(_durin_prefix IN LISTS DURIN_NATIVE_TEST_RESERVED_LABEL_PREFIXES)
			if(_durin_label MATCHES "^${_durin_prefix}")
				message(FATAL_ERROR
					"${target_name} label '${_durin_label}' uses reserved native-test "
					"prefix '${_durin_prefix}'. Declare structured metadata instead.")
			endif()
		endforeach()
	endforeach()
endfunction()

function(durin_finalize_native_test target_name)
	if(NOT TARGET ${target_name})
		message(FATAL_ERROR
			"Cannot finalize structured metadata for missing target ${target_name}.")
	endif()
	get_target_property(_durin_discovered ${target_name} DURIN_TEST_DISCOVERED)
	if(_durin_discovered)
		message(FATAL_ERROR
			"${target_name} structured metadata must be finalized before durin_discover_tests.")
	endif()

	set(one_value_args KIND PRIVATE_SOURCE_OWNER PRIVATE_SOURCE_RATIONALE)
	set(multi_value_args DOMAINS MODULES BACKENDS STACKS)
	cmake_parse_arguments(DURIN_METADATA "" "${one_value_args}" "${multi_value_args}" ${ARGN})
	if(DURIN_METADATA_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR
			"Unknown structured native-test metadata for ${target_name}: "
			"${DURIN_METADATA_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT DURIN_METADATA_KIND)
		message(FATAL_ERROR "${target_name} structured native-test metadata requires KIND.")
	endif()
	durin_validate_native_test_metadata_value(_durin_kind kind "${DURIN_METADATA_KIND}")
	if(NOT _durin_kind IN_LIST DURIN_NATIVE_TEST_KINDS)
		message(FATAL_ERROR
			"${target_name} KIND '${_durin_kind}' is invalid; expected one of: "
			"${DURIN_NATIVE_TEST_KINDS}.")
	endif()
	if(NOT DURIN_METADATA_DOMAINS)
		message(FATAL_ERROR "${target_name} structured native-test metadata requires DOMAINS.")
	endif()
	durin_normalize_native_test_metadata_list(
		_durin_domains domains ${DURIN_METADATA_DOMAINS})
	durin_normalize_native_test_metadata_list(
		_durin_modules modules ${DURIN_METADATA_MODULES})
	durin_normalize_native_test_metadata_list(
		_durin_backends backends ${DURIN_METADATA_BACKENDS})
	durin_normalize_native_test_metadata_list(
		_durin_stacks stacks ${DURIN_METADATA_STACKS})

	get_target_property(_durin_direct_lifecycle ${target_name} DURIN_TEST_DIRECT_LIFECYCLE)
	if(_durin_direct_lifecycle MATCHES "-NOTFOUND$")
		set(_durin_direct_lifecycle TRUE)
	endif()
	get_target_property(_durin_labels ${target_name} DURIN_TEST_LABELS)
	if(_durin_labels MATCHES "-NOTFOUND$")
		set(_durin_labels)
	endif()
	durin_assert_native_test_labels_not_reserved("${target_name}" ${_durin_labels})
	if(_durin_kind STREQUAL "characterization")
		if(_durin_direct_lifecycle)
			message(FATAL_ERROR
				"${target_name} KIND characterization requires DURIN_TEST_DIRECT_LIFECYCLE FALSE.")
		endif()
		list(APPEND _durin_labels native-test-characterization)
	elseif(native-test-characterization IN_LIST _durin_labels)
		message(FATAL_ERROR
			"${target_name} non-characterization KIND cannot use native-test-characterization.")
	endif()
	if(_durin_kind STREQUAL "qualification")
		list(APPEND _durin_labels native-test-qualification)
	elseif(native-test-qualification IN_LIST _durin_labels)
		message(FATAL_ERROR
			"${target_name} non-qualification KIND cannot use native-test-qualification.")
	endif()

	get_target_property(_durin_sources ${target_name} SOURCES)
	foreach(_durin_source IN LISTS _durin_sources)
		if(_durin_source MATCHES "^\\$<")
			continue()
		endif()
		if(IS_ABSOLUTE "${_durin_source}")
			cmake_path(NORMAL_PATH _durin_source OUTPUT_VARIABLE _durin_source_absolute)
		else()
			get_target_property(_durin_source_dir ${target_name} SOURCE_DIR)
			cmake_path(ABSOLUTE_PATH _durin_source BASE_DIRECTORY "${_durin_source_dir}"
				NORMALIZE OUTPUT_VARIABLE _durin_source_absolute)
		endif()
		set(_durin_is_test_owned_source FALSE)
		if(DEFINED DURIN_PROJECT_TESTS_DIR)
			cmake_path(IS_PREFIX DURIN_PROJECT_TESTS_DIR "${_durin_source_absolute}"
				NORMALIZE _durin_is_test_owned_source)
		endif()
		if(_durin_is_test_owned_source OR NOT _durin_source_absolute MATCHES "[/\\\\]Private[/\\\\].*\\.(c|cc|cpp|cxx)$")
			continue()
		endif()
		if(NOT DURIN_METADATA_PRIVATE_SOURCE_OWNER OR NOT DURIN_METADATA_PRIVATE_SOURCE_RATIONALE)
			message(FATAL_ERROR
				"${target_name} compiles production-private source ${_durin_source_absolute}. "
				"Link the production module or declare an owned seam with "
				"PRIVATE_SOURCE_OWNER and PRIVATE_SOURCE_RATIONALE.")
		endif()
		string(TOLOWER "${DURIN_METADATA_PRIVATE_SOURCE_OWNER}" _durin_private_owner)
		string(REGEX REPLACE "([a-z0-9])([A-Z])" "\\1-\\2"
			_durin_private_owner_slug "${DURIN_METADATA_PRIVATE_SOURCE_OWNER}")
		string(TOLOWER "${_durin_private_owner_slug}" _durin_private_owner_slug)
		if(NOT _durin_private_owner IN_LIST _durin_modules
			AND NOT _durin_private_owner_slug IN_LIST _durin_modules)
			message(FATAL_ERROR
				"${target_name} PRIVATE_SOURCE_OWNER must also appear in MODULES.")
		endif()
		if(NOT TARGET ${DURIN_METADATA_PRIVATE_SOURCE_OWNER})
			message(FATAL_ERROR
				"${target_name} PRIVATE_SOURCE_OWNER '${DURIN_METADATA_PRIVATE_SOURCE_OWNER}' "
				"is not a configured production module target.")
		endif()
		get_target_property(_durin_owner_source_dir
			${DURIN_METADATA_PRIVATE_SOURCE_OWNER} SOURCE_DIR)
		set(_durin_owner_private_dir "${_durin_owner_source_dir}/Private")
		cmake_path(IS_PREFIX _durin_owner_private_dir "${_durin_source_absolute}"
			NORMALIZE _durin_is_owned_private_source)
		if(NOT _durin_is_owned_private_source)
			message(FATAL_ERROR
				"${target_name} production-private source ${_durin_source_absolute} is not "
				"owned by ${DURIN_METADATA_PRIVATE_SOURCE_OWNER}.")
		endif()
	endforeach()

	set(_durin_structured_labels "kind-${_durin_kind}")
	foreach(_durin_dimension domains modules backends stacks)
		string(REGEX REPLACE "s$" "" _durin_label_prefix "${_durin_dimension}")
		foreach(_durin_value IN LISTS _durin_${_durin_dimension})
			list(APPEND _durin_structured_labels "${_durin_label_prefix}-${_durin_value}")
		endforeach()
	endforeach()
	list(APPEND _durin_labels ${_durin_structured_labels})
	list(REMOVE_DUPLICATES _durin_labels)
	set_target_properties(${target_name} PROPERTIES
		DURIN_TEST_KIND "${_durin_kind}"
		DURIN_TEST_DOMAINS "${_durin_domains}"
		DURIN_TEST_MODULES "${_durin_modules}"
		DURIN_TEST_BACKENDS "${_durin_backends}"
		DURIN_TEST_STACKS "${_durin_stacks}"
		DURIN_TEST_LABELS "${_durin_labels}"
		DURIN_TEST_PRIVATE_SOURCE_OWNER "${DURIN_METADATA_PRIVATE_SOURCE_OWNER}"
		DURIN_TEST_PRIVATE_SOURCE_RATIONALE "${DURIN_METADATA_PRIVATE_SOURCE_RATIONALE}"
	)
endfunction()

function(durin_validate_native_test_finalization target_name)
	if(NOT TARGET ${target_name})
		message(FATAL_ERROR
			"Cannot validate native-test finalization for missing target ${target_name}.")
	endif()
	get_target_property(_durin_kind ${target_name} DURIN_TEST_KIND)
	if(_durin_kind MATCHES "-NOTFOUND$" OR NOT _durin_kind)
		message(FATAL_ERROR
			"Native-test target ${target_name} must call durin_finalize_native_test "
			"with KIND and DOMAINS before durin_discover_tests.")
	endif()
endfunction()

function(durin_add_native_test_aggregate_target target_name)
	if(TARGET ${target_name})
		message(FATAL_ERROR "Native-test aggregate target ${target_name} already exists.")
	endif()

	get_property(_durin_native_test_targets GLOBAL PROPERTY DURIN_NATIVE_TEST_TARGETS)
	list(REMOVE_DUPLICATES _durin_native_test_targets)
	set(_durin_default_native_test_targets)
	foreach(_durin_native_test_target IN LISTS _durin_native_test_targets)
		get_target_property(_durin_native_test_kind
			${_durin_native_test_target} DURIN_TEST_KIND)
		if(_durin_native_test_kind STREQUAL "characterization"
			OR _durin_native_test_kind STREQUAL "qualification")
			continue()
		endif()
		list(APPEND _durin_default_native_test_targets
			${_durin_native_test_target})
	endforeach()

	add_custom_target(${target_name})
	if(_durin_default_native_test_targets)
		add_dependencies(${target_name} ${_durin_default_native_test_targets})
	endif()
	set_target_properties(${target_name} PROPERTIES FOLDER "Tests")
	if(DEFINED ENV{DURIN_NATIVE_TEST_RUNTIME_CLOSURE_AUDIT})
		durin_report_target_dependency_expression_audit()
	endif()
endfunction()

function(durin_exclude_native_test_sources)
	set(one_value_args RATIONALE)
	set(multi_value_args SOURCES)
	cmake_parse_arguments(
		DURIN_NATIVE_TEST_EXCLUSION
		""
		"${one_value_args}"
		"${multi_value_args}"
		${ARGN}
	)
	if(NOT DURIN_NATIVE_TEST_EXCLUSION_RATIONALE)
		message(FATAL_ERROR
			"Native-test source exclusions require a reviewed RATIONALE.")
	endif()
	if(NOT DURIN_NATIVE_TEST_EXCLUSION_SOURCES)
		message(FATAL_ERROR "Native-test source exclusions require SOURCES.")
	endif()

	foreach(_durin_test_source IN LISTS DURIN_NATIVE_TEST_EXCLUSION_SOURCES)
		cmake_path(ABSOLUTE_PATH _durin_test_source
			BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			NORMALIZE
			OUTPUT_VARIABLE _durin_test_source_absolute)
		if(NOT EXISTS "${_durin_test_source_absolute}")
			message(FATAL_ERROR
				"Excluded native-test source does not exist: "
				"${_durin_test_source_absolute}")
		endif()
		get_property(_durin_existing_owner GLOBAL
			PROPERTY "DURIN_NATIVE_TEST_SOURCE_OWNER_${_durin_test_source_absolute}")
		if(_durin_existing_owner)
			message(FATAL_ERROR
				"Native-test source ${_durin_test_source_absolute} is already owned by "
				"${_durin_existing_owner}.")
		endif()
		set_property(GLOBAL APPEND PROPERTY DURIN_OWNED_NATIVE_TEST_SOURCES
			"${_durin_test_source_absolute}")
		set_property(GLOBAL
			PROPERTY "DURIN_NATIVE_TEST_SOURCE_OWNER_${_durin_test_source_absolute}"
			"configuration exclusion: ${DURIN_NATIVE_TEST_EXCLUSION_RATIONALE}")
	endforeach()
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
		if(_durin_cmake_content MATCHES
			"add_custom_command[ \\t\\r\\n]*\\([^)]*TARGET[^)]*POST_BUILD[^)]*(copy|copy_if_different)")
			message(FATAL_ERROR
				"${_durin_cmake_file} adds a target-owned POST_BUILD runtime copy. "
				"Use the shared native-test runtime deployment helpers.")
		endif()
	endforeach()
endfunction()

set(DURIN_NATIVE_TEST_RESOURCE_LOCK_REGISTRY
	durin-gpu
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
	list(REMOVE_DUPLICATES _durin_resource_locks)

	set(_durin_labels native-test "${target_name}" ${DURIN_TEST_POLICY_LABELS})
	list(REMOVE_DUPLICATES _durin_labels)

	set(${out_resource_locks} "${_durin_resource_locks}" PARENT_SCOPE)
	set(${out_labels} "${_durin_labels}" PARENT_SCOPE)
endfunction()

function(durin_resolve_native_test_execution_policy
	out_case_labels
	out_target_labels
	target_name
	direct_lifecycle)
	set(options)
	set(one_value_args)
	set(multi_value_args LABELS)
	cmake_parse_arguments(
		DURIN_EXECUTION
		"${options}"
		"${one_value_args}"
		"${multi_value_args}"
		${ARGN}
	)
	if(DURIN_EXECUTION_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR
			"Unknown native-test execution policy arguments for ${target_name}: "
			"${DURIN_EXECUTION_UNPARSED_ARGUMENTS}")
	endif()

	set(_durin_case_labels ${DURIN_EXECUTION_LABELS} native-test-case)
	list(REMOVE_DUPLICATES _durin_case_labels)
	set(_durin_target_labels)
	if(native-test-characterization IN_LIST DURIN_EXECUTION_LABELS)
		if(direct_lifecycle)
			message(FATAL_ERROR
				"${target_name} characterization tests cannot register a direct lifecycle.")
		endif()
		set(${out_case_labels} "${_durin_case_labels}" PARENT_SCOPE)
		set(${out_target_labels} "" PARENT_SCOPE)
		return()
	endif()

	if(NOT direct_lifecycle)
		message(FATAL_ERROR
			"${target_name} ordinary native tests require a direct lifecycle registration.")
	endif()
	set(_durin_target_labels
		${DURIN_EXECUTION_LABELS}
		native-test-target
		native-test-direct
		native-test-default
	)
	list(REMOVE_DUPLICATES _durin_case_labels)
	list(REMOVE_DUPLICATES _durin_target_labels)
	set(${out_case_labels} "${_durin_case_labels}" PARENT_SCOPE)
	set(${out_target_labels} "${_durin_target_labels}" PARENT_SCOPE)
endfunction()

function(durin_json_escape out_value value)
	set(_durin_value "${value}")
	string(REPLACE "\\" "\\\\" _durin_value "${_durin_value}")
	string(REPLACE "\"" "\\\"" _durin_value "${_durin_value}")
	string(REPLACE "\n" "\\n" _durin_value "${_durin_value}")
	string(REPLACE "\r" "\\r" _durin_value "${_durin_value}")
	string(REPLACE "\t" "\\t" _durin_value "${_durin_value}")
	set(${out_value} "${_durin_value}" PARENT_SCOPE)
endfunction()

function(durin_json_string_array out_value)
	set(_durin_items)
	foreach(_durin_value IN LISTS ARGN)
		durin_json_escape(_durin_escaped "${_durin_value}")
		list(APPEND _durin_items "\"${_durin_escaped}\"")
	endforeach()
	string(JOIN "," _durin_joined ${_durin_items})
	set(${out_value} "[${_durin_joined}]" PARENT_SCOPE)
endfunction()

function(durin_generate_native_test_registry output_path)
	get_property(_durin_targets GLOBAL PROPERTY DURIN_NATIVE_TEST_TARGETS)
	list(REMOVE_DUPLICATES _durin_targets)
	list(SORT _durin_targets)
	set(_durin_records)
	foreach(_durin_target IN LISTS _durin_targets)
		get_target_property(_durin_discovered ${_durin_target} DURIN_TEST_DISCOVERED)
		if(NOT _durin_discovered)
			message(FATAL_ERROR
				"Native-test target ${_durin_target} was not finalized with durin_discover_tests.")
		endif()
		foreach(_durin_property
			KIND DOMAINS MODULES BACKENDS STACKS
			DIRECT_LIFECYCLE TIMEOUT DISCOVERY_RESOURCE_LOCKS
			HEAVY_RUNTIME_RATIONALE PRIVATE_SOURCE_OWNER PRIVATE_SOURCE_RATIONALE)
			get_target_property(_durin_${_durin_property}
				${_durin_target} DURIN_TEST_${_durin_property})
			if(_durin_${_durin_property} MATCHES "-NOTFOUND$")
				set(_durin_${_durin_property})
			endif()
		endforeach()
		foreach(_durin_list DOMAINS MODULES BACKENDS STACKS DISCOVERY_RESOURCE_LOCKS)
			durin_json_string_array(_durin_${_durin_list}_json ${_durin_${_durin_list}})
		endforeach()
		if(_durin_DIRECT_LIFECYCLE)
			set(_durin_direct_json true)
		else()
			set(_durin_direct_json false)
		endif()
		if(_durin_HEAVY_RUNTIME_RATIONALE)
			set(_durin_heavy_json true)
		else()
			set(_durin_heavy_json false)
		endif()
		durin_json_escape(_durin_name_json "${_durin_target}")
		durin_json_escape(_durin_kind_json "${_durin_KIND}")
		durin_json_escape(_durin_private_source_owner_json "${_durin_PRIVATE_SOURCE_OWNER}")
		durin_json_escape(_durin_private_source_rationale_json "${_durin_PRIVATE_SOURCE_RATIONALE}")
		string(CONCAT _durin_record
			"    {\"name\":\"${_durin_name_json}\",\"availability\":\"configured\","
			"\"kind\":\"${_durin_kind_json}\","
			"\"domains\":${_durin_DOMAINS_json},\"modules\":${_durin_MODULES_json},"
			"\"backends\":${_durin_BACKENDS_json},\"stacks\":${_durin_STACKS_json},"
			"\"directLifecycle\":${_durin_direct_json},\"timeoutSeconds\":${_durin_TIMEOUT},"
			"\"resourceLocks\":${_durin_DISCOVERY_RESOURCE_LOCKS_json},"
			"\"heavyRuntime\":${_durin_heavy_json},"
			"\"privateSourceOwner\":\"${_durin_private_source_owner_json}\","
			"\"privateSourceRationale\":\"${_durin_private_source_rationale_json}\"}")
		list(APPEND _durin_records "${_durin_record}")
	endforeach()
	string(JOIN ",\n" _durin_records_json ${_durin_records})
	durin_json_escape(_durin_source_dir_json "${CMAKE_SOURCE_DIR}")
	durin_json_escape(_durin_binary_dir_json "${CMAKE_BINARY_DIR}")
	set(_durin_registry_preset "${CMAKE_PRESET_NAME}")
	if(NOT _durin_registry_preset)
		get_filename_component(_durin_registry_preset "${CMAKE_BINARY_DIR}" NAME)
	endif()
	durin_json_escape(_durin_preset_json "${_durin_registry_preset}")
	durin_json_escape(_durin_configuration_json "${CMAKE_BUILD_TYPE}")
	string(CONCAT _durin_registry
		"{\n"
		"  \"schemaVersion\": 2,\n"
		"  \"identity\": {\"sourceDir\":\"${_durin_source_dir_json}\","
		"\"binaryDir\":\"${_durin_binary_dir_json}\",\"preset\":\"${_durin_preset_json}\","
		"\"configuration\":\"${_durin_configuration_json}\"},\n"
		"  \"targets\": [\n${_durin_records_json}\n  ]\n"
		"}\n")
	get_filename_component(_durin_registry_dir "${output_path}" DIRECTORY)
	file(MAKE_DIRECTORY "${_durin_registry_dir}")
	set(_durin_temp_path "${output_path}.tmp")
	file(WRITE "${_durin_temp_path}" "${_durin_registry}")
	file(RENAME "${_durin_temp_path}" "${output_path}")
	set_property(GLOBAL PROPERTY DURIN_NATIVE_TEST_REGISTRY_PATH "${output_path}")
	list(LENGTH _durin_targets _durin_target_count)
	message(STATUS
		"Generated native-test registry (${_durin_target_count} targets): ${output_path}")
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
	durin_validate_native_test_finalization(${target_name})

	# Discovery is the first point at which the test's link declarations are
	# complete. Register its complete deployable runtime closure before
	# GoogleTest adds the post-link discovery command.
	durin_test_register_runtime_dependency_closure("${target_name}")

	get_target_property(_durin_case_parallel_safe
		${target_name} DURIN_TEST_CASE_PARALLEL_SAFE)
	if(_durin_case_parallel_safe MATCHES "-NOTFOUND$")
		set(_durin_case_parallel_safe FALSE)
	elseif(NOT _durin_case_parallel_safe STREQUAL "TRUE"
		AND NOT _durin_case_parallel_safe STREQUAL "FALSE")
		message(FATAL_ERROR
			"${target_name} DURIN_TEST_CASE_PARALLEL_SAFE must be TRUE or FALSE.")
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
	get_target_property(_durin_direct_lifecycle
		${target_name} DURIN_TEST_DIRECT_LIFECYCLE)
	if(_durin_direct_lifecycle MATCHES "-NOTFOUND$")
		set(_durin_direct_lifecycle TRUE)
	elseif(NOT _durin_direct_lifecycle STREQUAL "TRUE"
		AND NOT _durin_direct_lifecycle STREQUAL "FALSE")
		message(FATAL_ERROR
			"${target_name} DURIN_TEST_DIRECT_LIFECYCLE must be TRUE or FALSE.")
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
		RESOURCE_LOCKS ${_durin_explicit_locks}
		LABELS ${_durin_extra_labels}
		TARGET_LOCK_RATIONALE "${_durin_target_lock_rationale}"
	)
	durin_resolve_native_test_execution_policy(
		_durin_case_labels
		_durin_target_labels
		"${target_name}"
		"${_durin_direct_lifecycle}"
		LABELS ${_durin_labels}
	)

	set_target_properties(${target_name} PROPERTIES
		DURIN_TEST_DISCOVERY_RESOURCE_LOCKS "${_durin_resource_locks}"
		DURIN_TEST_DISCOVERY_LABELS "${_durin_case_labels}"
		DURIN_TEST_DIRECT_LIFECYCLE "${_durin_direct_lifecycle}"
		DURIN_TEST_TIMEOUT "${_durin_timeout}"
		DURIN_TEST_DISCOVERED TRUE
	)

	gtest_discover_tests(${target_name}
		WORKING_DIRECTORY "${_durin_work_dir}"
		DISCOVERY_TIMEOUT 30
		PROPERTIES TIMEOUT "${_durin_timeout}"
	)

	# GoogleTest's discovery helper loses the grouping of semicolon-separated
	# property values while forwarding them through its generated -D argument.
	# Apply list-valued policy from a CTest include after discovery instead.
	set(_durin_policy_file
		"${CMAKE_CURRENT_BINARY_DIR}/${target_name}-durin-test-policy.cmake")
	string(CONCAT _durin_policy_content
		"foreach(_durin_discovered_test IN LISTS ${target_name}_TESTS)\n"
		"  set_tests_properties(\"\${_durin_discovered_test}\" PROPERTIES\n"
		"    LABELS \"${_durin_case_labels}\"\n")
	if(_durin_resource_locks)
		string(APPEND _durin_policy_content
			"    RESOURCE_LOCK \"${_durin_resource_locks}\"\n")
	endif()
	string(APPEND _durin_policy_content
		"  )\n"
		"endforeach()\n")
	file(GENERATE
		OUTPUT "${_durin_policy_file}"
		CONTENT "${_durin_policy_content}"
	)
	set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
		"${_durin_policy_file}")

	if(_durin_direct_lifecycle)
		add_test(
			NAME "Durin.NativeTestDirect.${target_name}"
			COMMAND "$<TARGET_FILE:${target_name}>" --gtest_brief=1
			WORKING_DIRECTORY "${_durin_work_dir}"
		)
		set_tests_properties(
			"Durin.NativeTestDirect.${target_name}"
			PROPERTIES
				TIMEOUT "${_durin_timeout}"
				LABELS "${_durin_target_labels}"
		)
		if(_durin_resource_locks)
			set_tests_properties(
				"Durin.NativeTestDirect.${target_name}"
				PROPERTIES
					RESOURCE_LOCK "${_durin_resource_locks}"
			)
		endif()
	endif()
endfunction()
