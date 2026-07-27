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

durin_resolve_native_test_discovery_policy(
	default_locks
	default_labels
	CoreUtilityTests
	FALSE
	""
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
	RESOURCE_LOCKS durin-gpu fixed-port
	LABELS integration
)
assert_list_equals(
	"${grouped_locks}"
	"durin-gpu;fixed-port;durin-test-legacy-renderer-runtime"
	"explicit and legacy locks"
)
assert_list_equals(
	"${grouped_labels}"
	"native-test;TextureCookIntegrationTests;integration"
	"grouped labels"
)
