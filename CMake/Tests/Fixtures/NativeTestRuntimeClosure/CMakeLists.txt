cmake_minimum_required(VERSION 3.24)

project(DurinNativeTestRuntimeClosureFixture LANGUAGES CXX)

if(NOT DEFINED DURIN_WORKSPACE_DIR)
	message(FATAL_ERROR "DURIN_WORKSPACE_DIR is required.")
endif()
if(NOT DEFINED DURIN_CLOSURE_PROBE)
	set(DURIN_CLOSURE_PROBE success)
endif()

include("${DURIN_WORKSPACE_DIR}/CMake/Project/ProjectOutputs.cmake")

set(_durin_empty_source "${CMAKE_CURRENT_BINARY_DIR}/empty.cpp")
file(WRITE "${_durin_empty_source}" "int DurinRuntimeClosureFixture = 0;\n")

add_library(shared_leaf SHARED "${_durin_empty_source}")
add_library(shared_leaf_alias ALIAS shared_leaf)
add_library(debug_shared SHARED "${_durin_empty_source}")
add_library(release_shared SHARED "${_durin_empty_source}")
add_library(optional_shared SHARED "${_durin_empty_source}")
add_library(runtime_module MODULE "${_durin_empty_source}")
add_library(static_bridge STATIC "${_durin_empty_source}")
target_link_libraries(static_bridge PRIVATE shared_leaf_alias)
add_library(interface_bridge INTERFACE)
target_link_libraries(interface_bridge INTERFACE static_bridge)
add_library(object_shared SHARED "${_durin_empty_source}")
add_library(object_bridge OBJECT "${_durin_empty_source}")
target_link_libraries(object_bridge PRIVATE object_shared)

add_library(cycle_a STATIC "${_durin_empty_source}")
add_library(cycle_b STATIC "${_durin_empty_source}")
target_link_libraries(cycle_a PRIVATE cycle_b)
target_link_libraries(cycle_b PRIVATE cycle_a)

set(_durin_external_dir "${CMAKE_CURRENT_BINARY_DIR}/external")
file(MAKE_DIRECTORY "${_durin_external_dir}")
set(_durin_external_runtime "${_durin_external_dir}/external-runtime.dll")
set(_durin_external_import "${_durin_external_dir}/external-runtime.lib")
file(WRITE "${_durin_external_runtime}" "external runtime\n")
file(WRITE "${_durin_external_import}" "external import\n")
add_library(external_runtime SHARED IMPORTED GLOBAL)
set_target_properties(external_runtime PROPERTIES
	IMPORTED_LOCATION "${_durin_external_runtime}"
	IMPORTED_IMPLIB "${_durin_external_import}"
	DURIN_RUNTIME_DEPLOY_FILES "${_durin_external_runtime}"
)

add_executable(closure_probe "${_durin_empty_source}")
set_target_properties(closure_probe PROPERTIES
	DURIN_TEST_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/Bin"
)
target_link_libraries(closure_probe PRIVATE
	interface_bridge
	object_bridge
	cycle_a
	"$<TARGET_NAME_IF_EXISTS:optional_shared>"
	debug debug_shared
	optimized release_shared
	external_runtime
)

set(_durin_runtime_only_file
	"${CMAKE_CURRENT_BINARY_DIR}/runtime-only/plugin-data.bin")
get_filename_component(_durin_runtime_only_dir
	"${_durin_runtime_only_file}" DIRECTORY)
file(MAKE_DIRECTORY "${_durin_runtime_only_dir}")
file(WRITE "${_durin_runtime_only_file}" "runtime-only data\n")

if(DURIN_CLOSURE_PROBE STREQUAL "missing-runtime-target")
	durin_test_register_runtime_only_dependencies(closure_probe
		RATIONALE "Exercises missing runtime-only target diagnostics."
		TARGETS missing_runtime_module
	)
elseif(DURIN_CLOSURE_PROBE STREQUAL "linked-runtime-only")
	durin_test_register_runtime_only_dependencies(closure_probe
		RATIONALE "Exercises linked target rejection."
		TARGETS shared_leaf
	)
	durin_test_collect_runtime_dependency_closure(
		closure_probe deploy_targets deploy_files visited_targets linked_targets)
elseif(DURIN_CLOSURE_PROBE STREQUAL "unknown-expression")
	target_link_libraries(closure_probe PRIVATE
		"$<UNKNOWN_TARGET_WRAPPER:shared_leaf>")
	durin_test_collect_runtime_dependency_closure(
		closure_probe deploy_targets deploy_files visited_targets linked_targets)
elseif(DURIN_CLOSURE_PROBE STREQUAL "destination-collision")
	set(_durin_collision_a "${CMAKE_CURRENT_BINARY_DIR}/a/runtime.dll")
	set(_durin_collision_b "${CMAKE_CURRENT_BINARY_DIR}/b/runtime.dll")
	get_filename_component(_durin_collision_a_dir "${_durin_collision_a}" DIRECTORY)
	get_filename_component(_durin_collision_b_dir "${_durin_collision_b}" DIRECTORY)
	file(MAKE_DIRECTORY "${_durin_collision_a_dir}" "${_durin_collision_b_dir}")
	file(WRITE "${_durin_collision_a}" "a\n")
	file(WRITE "${_durin_collision_b}" "b\n")
	durin_test_get_runtime_file_deployment(
		"${_durin_collision_a}" "${CMAKE_CURRENT_BINARY_DIR}/Bin" first_deployment)
	durin_test_get_runtime_file_deployment(
		"${_durin_collision_b}" "${CMAKE_CURRENT_BINARY_DIR}/Bin" second_deployment)
elseif(DURIN_CLOSURE_PROBE STREQUAL "manual-duplicate")
	durin_test_deploy_target_binary(closure_probe shared_leaf)
	durin_test_register_runtime_dependency_closure(closure_probe)
elseif(DURIN_CLOSURE_PROBE STREQUAL "manual-file-duplicate")
	durin_test_deploy_runtime_files(closure_probe external_runtime)
	durin_test_register_runtime_dependency_closure(closure_probe)
elseif(DURIN_CLOSURE_PROBE STREQUAL "success")
	durin_test_register_runtime_only_dependencies(closure_probe
		RATIONALE "Exercises an unlinked plugin module and data file."
		TARGETS runtime_module
		FILES "${_durin_runtime_only_file}"
	)
	durin_test_register_runtime_dependency_closure(closure_probe)
	get_target_property(deploy_targets closure_probe
		DURIN_TEST_DERIVED_RUNTIME_TARGETS)
	get_target_property(deploy_files closure_probe
		DURIN_TEST_DERIVED_RUNTIME_FILES)
	get_target_property(visited_targets closure_probe
		DURIN_TEST_RUNTIME_VISITED_TARGETS)

	add_executable(closure_probe_two "${_durin_empty_source}")
	set_target_properties(closure_probe_two PROPERTIES
		DURIN_TEST_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/Bin"
	)
	target_link_libraries(closure_probe_two PRIVATE
		shared_leaf external_runtime)
	durin_test_register_runtime_dependency_closure(closure_probe_two)
	durin_test_get_target_binary_deployment(
		shared_leaf "${CMAKE_CURRENT_BINARY_DIR}/Bin" first_binary_deployment)
	durin_test_get_target_binary_deployment(
		shared_leaf "${CMAKE_CURRENT_BINARY_DIR}/Bin" second_binary_deployment)
	if(NOT first_binary_deployment STREQUAL second_binary_deployment)
		message(FATAL_ERROR
			"Shared target deployment did not reuse one writer.")
	endif()
	durin_test_get_runtime_file_deployment(
		"${_durin_external_runtime}"
		"${CMAKE_CURRENT_BINARY_DIR}/Bin"
		first_file_deployment
	)
	durin_test_get_runtime_file_deployment(
		"${_durin_external_runtime}"
		"${CMAKE_CURRENT_BINARY_DIR}/Bin"
		second_file_deployment
	)
	if(NOT first_file_deployment STREQUAL second_file_deployment)
		message(FATAL_ERROR
			"Shared file deployment did not reuse one writer.")
	endif()

	set(expected_deploy_targets
		debug_shared object_shared optional_shared runtime_module shared_leaf)
	list(SORT expected_deploy_targets)
	if(NOT deploy_targets STREQUAL expected_deploy_targets)
		message(FATAL_ERROR
			"Unexpected deploy targets: expected '${expected_deploy_targets}', "
			"got '${deploy_targets}'.")
	endif()

	set(expected_deploy_files
		"${_durin_external_runtime}" "${_durin_runtime_only_file}")
	list(SORT expected_deploy_files)
	if(NOT deploy_files STREQUAL expected_deploy_files)
		message(FATAL_ERROR
			"Unexpected deploy files: expected '${expected_deploy_files}', "
			"got '${deploy_files}'.")
	endif()

	set(expected_visited_targets
		closure_probe cycle_a cycle_b debug_shared external_runtime
		interface_bridge object_bridge object_shared optional_shared runtime_module
		shared_leaf static_bridge)
	list(SORT expected_visited_targets)
	if(NOT visited_targets STREQUAL expected_visited_targets)
		message(FATAL_ERROR
			"Unexpected visited targets: expected '${expected_visited_targets}', "
			"got '${visited_targets}'.")
	endif()

	if(release_shared IN_LIST visited_targets)
		message(FATAL_ERROR
			"Release-only dependency appeared in the Debug closure.")
	endif()
else()
	message(FATAL_ERROR "Unknown closure probe '${DURIN_CLOSURE_PROBE}'.")
endif()
