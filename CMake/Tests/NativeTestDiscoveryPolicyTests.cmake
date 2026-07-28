cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED DURIN_WORKSPACE_DIR)
	message(FATAL_ERROR "DURIN_WORKSPACE_DIR is required.")
endif()

include("${DURIN_WORKSPACE_DIR}/CMake/Project/ProjectTargets.cmake")

function(assert_list_equals actual expected description)
	if(NOT "${actual}" STREQUAL "${expected}")
		message(FATAL_ERROR
			"${description}: expected '${expected}', got '${actual}'.")
	endif()
endfunction()

function(assert_policy_rejected probe expected_text)
	execute_process(
		COMMAND "${CMAKE_COMMAND}"
			"-DDURIN_WORKSPACE_DIR=${DURIN_WORKSPACE_DIR}"
			"-DDURIN_POLICY_PROBE=${probe}"
			"-DDURIN_PROBE_ROOT=${DURIN_TEST_BINARY_DIR}/PolicyProbe/${probe}"
			-P "${DURIN_WORKSPACE_DIR}/CMake/Tests/NativeTestPolicyFailureProbe.cmake"
		RESULT_VARIABLE probe_result
		OUTPUT_VARIABLE probe_output
		ERROR_VARIABLE probe_error
	)
	if(probe_result EQUAL 0)
		message(FATAL_ERROR
			"Policy probe '${probe}' unexpectedly succeeded.")
	endif()
	set(probe_text "${probe_output}\n${probe_error}")
	if(NOT probe_text MATCHES "${expected_text}")
		message(FATAL_ERROR
			"Policy probe '${probe}' did not report '${expected_text}':\n"
			"${probe_text}")
	endif()
endfunction()

durin_resolve_native_test_discovery_policy(
	default_locks
	default_labels
	CoreUtilityTests
	FALSE
	""
	TARGET_LOCK_RATIONALE "Characterization of the explicit broad-lock policy."
)
assert_list_equals(
	"${default_locks}"
	"durin-test-target-CoreUtilityTests"
	"default target serialization"
)
assert_list_equals(
	"${default_labels}"
	"native-test;CoreUtilityTests"
	"default labels"
)

durin_resolve_native_test_discovery_policy(
	parallel_locks
	parallel_labels
	CoreConcurrencyTests
	TRUE
	""
	LABELS fast core
)
assert_list_equals("${parallel_locks}" "" "parallel-safe target lock")
assert_list_equals(
	"${parallel_labels}"
	"native-test;CoreConcurrencyTests;fast;core"
	"parallel-safe labels"
)

durin_resolve_native_test_discovery_policy(
	grouped_locks
	grouped_labels
	TextureCookIntegrationTests
	TRUE
	renderer-runtime
	RESOURCE_LOCKS durin-gpu
	LABELS integration
)
assert_list_equals(
	"${grouped_locks}"
	"durin-gpu;durin-test-legacy-renderer-runtime"
	"explicit and legacy locks"
)
assert_list_equals(
	"${grouped_labels}"
	"native-test;TextureCookIntegrationTests;integration"
	"grouped labels"
)
assert_list_equals(
	"${DURIN_NATIVE_TEST_RESOURCE_LOCK_REGISTRY}"
	"durin-gpu"
	"documented explicit resource registry"
)
assert_list_equals(
	"${DURIN_NATIVE_TEST_LEGACY_RESOURCE_GROUP_REGISTRY}"
	"renderer-runtime"
	"documented legacy resource registry"
)

assert_policy_rejected("unknown-resource" "unregistered native-test resource")
assert_policy_rejected("broad-lock-without-rationale" "TARGET_LOCK_RATIONALE")
assert_policy_rejected("repository-retired-work" "retired DURIN_TEST_WORK_DIR")
assert_policy_rejected("repository-direct-remove-all" "RemoveTestWorkDirectory")
assert_policy_rejected("repository-data-write" "mutate checked-in test Data")
assert_policy_rejected("repository-direct-discovery" "registers GoogleTest cases directly")
