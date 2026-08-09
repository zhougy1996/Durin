cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED DURIN_WORKSPACE_DIR)
	message(FATAL_ERROR "DURIN_WORKSPACE_DIR is required.")
endif()

include("${DURIN_WORKSPACE_DIR}/CMake/Project/ProjectTargets.cmake")

if(DURIN_POLICY_PROBE STREQUAL "unknown-resource")
	durin_resolve_native_test_discovery_policy(
		probe_locks
		probe_labels
		ProbeTests
		TRUE
		""
		RESOURCE_LOCKS unregistered-resource
	)
elseif(DURIN_POLICY_PROBE STREQUAL "broad-lock-without-rationale")
	durin_resolve_native_test_discovery_policy(
		probe_locks
		probe_labels
		ProbeTests
		FALSE
		""
	)
elseif(DURIN_POLICY_PROBE STREQUAL "execution-unknown-granularity")
	durin_resolve_native_test_execution_policy(
		probe_case_labels probe_target_labels ProbeTests TRUE PROCESS
		"Pending migration." "Stage 3" LABELS native-test ProbeTests)
elseif(DURIN_POLICY_PROBE STREQUAL "execution-case-missing-rationale")
	durin_resolve_native_test_execution_policy(
		probe_case_labels probe_target_labels ProbeTests TRUE CASE
		"" "Stage 3" LABELS native-test ProbeTests)
elseif(DURIN_POLICY_PROBE STREQUAL "execution-case-missing-stage")
	durin_resolve_native_test_execution_policy(
		probe_case_labels probe_target_labels ProbeTests TRUE CASE
		"Pending migration." "" LABELS native-test ProbeTests)
elseif(DURIN_POLICY_PROBE STREQUAL "execution-case-invalid-stage")
	durin_resolve_native_test_execution_policy(
		probe_case_labels probe_target_labels ProbeTests TRUE CASE
		"Pending migration." "Later" LABELS native-test ProbeTests)
elseif(DURIN_POLICY_PROBE STREQUAL "execution-target-with-case-metadata")
	durin_resolve_native_test_execution_policy(
		probe_case_labels probe_target_labels ProbeTests TRUE TARGET
		"Stale migration metadata." "Stage 3" LABELS native-test ProbeTests)
elseif(DURIN_POLICY_PROBE STREQUAL "execution-ordinary-without-direct")
	durin_resolve_native_test_execution_policy(
		probe_case_labels probe_target_labels ProbeTests FALSE CASE
		"Pending migration." "Stage 3" LABELS native-test ProbeTests)
elseif(DURIN_POLICY_PROBE STREQUAL "execution-characterization-with-default")
	durin_resolve_native_test_execution_policy(
		probe_case_labels probe_target_labels ProbeTests FALSE CASE
		"Pending migration." "Stage 3"
		LABELS native-test ProbeTests native-test-characterization)
elseif(DURIN_POLICY_PROBE MATCHES "^repository-")
	if(NOT DEFINED DURIN_PROBE_ROOT)
		message(FATAL_ERROR "DURIN_PROBE_ROOT is required.")
	endif()
	file(REMOVE_RECURSE "${DURIN_PROBE_ROOT}")
	file(MAKE_DIRECTORY "${DURIN_PROBE_ROOT}/Probe")
	if(DURIN_POLICY_PROBE STREQUAL "repository-retired-work")
		file(WRITE "${DURIN_PROBE_ROOT}/Probe/ProbeTests.cpp"
			"auto Root = DURIN_TEST_WORK_DIR;\n")
	elseif(DURIN_POLICY_PROBE STREQUAL "repository-direct-remove-all")
		file(WRITE "${DURIN_PROBE_ROOT}/Probe/ProbeTests.cpp"
			"std::filesystem::remove_all(Path);\n")
	elseif(DURIN_POLICY_PROBE STREQUAL "repository-data-write")
		file(WRITE "${DURIN_PROBE_ROOT}/Probe/ProbeTests.cpp"
			"std::ofstream Output(std::filesystem::path(DURIN_TEST_DATA_DIR) / \"changed\");\n")
	elseif(DURIN_POLICY_PROBE STREQUAL "repository-direct-discovery")
		file(WRITE "${DURIN_PROBE_ROOT}/Probe/CMakeLists.txt"
			"gtest_discover_tests(ProbeTests)\n")
	elseif(DURIN_POLICY_PROBE STREQUAL "repository-post-build-runtime-copy")
		file(WRITE "${DURIN_PROBE_ROOT}/Probe/CMakeLists.txt"
			"add_custom_command(TARGET ProbeTests POST_BUILD\n"
			"  COMMAND \\${CMAKE_COMMAND} -E copy_if_different source.dll destination.dll)\n")
	else()
		message(FATAL_ERROR "Unknown repository policy probe.")
	endif()
	durin_validate_native_test_repository_policy("${DURIN_PROBE_ROOT}")
else()
	message(FATAL_ERROR "Unknown DURIN_POLICY_PROBE '${DURIN_POLICY_PROBE}'.")
endif()

message(FATAL_ERROR
	"Policy probe '${DURIN_POLICY_PROBE}' unexpectedly succeeded.")
