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

function(configure_metadata_probe probe expect_success expected_text)
	set(_durin_probe_binary "${DURIN_TEST_BINARY_DIR}/MetadataProbe/${probe}")
	file(REMOVE_RECURSE "${_durin_probe_binary}")
	execute_process(
		COMMAND "${CMAKE_COMMAND}"
			-G Ninja
			"-DCMAKE_MAKE_PROGRAM=${DURIN_MAKE_PROGRAM}"
			-S "${DURIN_WORKSPACE_DIR}/CMake/Tests/Fixtures/NativeTestMetadata"
			-B "${_durin_probe_binary}"
			"-DDURIN_WORKSPACE_DIR=${DURIN_WORKSPACE_DIR}"
			"-DDURIN_METADATA_PROBE=${probe}"
		RESULT_VARIABLE _durin_result
		OUTPUT_VARIABLE _durin_output
		ERROR_VARIABLE _durin_error
	)
	if(expect_success)
		if(NOT _durin_result EQUAL 0)
			message(FATAL_ERROR
				"Metadata probe '${probe}' failed:\n${_durin_output}\n${_durin_error}")
		endif()
		if(probe STREQUAL "unavailable")
			set(_durin_expected_target_count 0)
		else()
			set(_durin_expected_target_count 1)
		endif()
		if(NOT _durin_output MATCHES
			"Generated native-test registry [(]${_durin_expected_target_count} targets[)]:")
			message(FATAL_ERROR
				"Metadata probe '${probe}' omitted the concise registry summary:\n"
				"${_durin_output}")
		endif()
		if(_durin_output MATCHES "registry for ProbeTests")
			message(FATAL_ERROR
				"Metadata probe '${probe}' listed registry target names:\n"
				"${_durin_output}")
		endif()
		set(_durin_registry "${_durin_probe_binary}/DurinNativeTestRegistry.json")
		file(READ "${_durin_registry}" _durin_first_registry)
		if(probe STREQUAL "unavailable")
			if(_durin_first_registry MATCHES "\"name\"")
				message(FATAL_ERROR "Unavailable metadata probe emitted a target record.")
			endif()
		elseif(NOT _durin_first_registry MATCHES "\"metadataMode\":\"structured\"")
			message(FATAL_ERROR "Metadata probe registry omitted structured mode.")
		endif()
		configure_file("${_durin_registry}" "${_durin_registry}.copy" COPYONLY)
		file(READ "${_durin_registry}.copy" _durin_second_registry)
		assert_list_equals("${_durin_first_registry}" "${_durin_second_registry}"
			"deterministic metadata registry")
	elseif(_durin_result EQUAL 0)
		message(FATAL_ERROR "Metadata probe '${probe}' unexpectedly succeeded.")
	else()
		set(_durin_text "${_durin_output}\n${_durin_error}")
		if(NOT _durin_text MATCHES "${expected_text}")
			message(FATAL_ERROR
				"Metadata probe '${probe}' did not report '${expected_text}':\n${_durin_text}")
		endif()
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

durin_resolve_native_test_execution_policy(
	case_default_case_labels
	case_default_target_labels
	PilotTests
	TRUE
	CASE
	"Stage 3 completes pilot qualification."
	"Stage 3"
	LABELS native-test PilotTests fast
)
assert_list_equals(
	"${case_default_case_labels}"
	"native-test;PilotTests;fast;native-test-case;native-test-default"
	"case-default discovered labels"
)
assert_list_equals(
	"${case_default_target_labels}"
	"native-test;PilotTests;fast;native-test-target;native-test-direct"
	"case-default direct labels"
)

durin_resolve_native_test_execution_policy(
	fallback_case_labels
	fallback_target_labels
	FallbackTests
	TRUE
	""
	""
	""
	LABELS native-test FallbackTests
)
assert_list_equals(
	"${fallback_case_labels}"
	"native-test;FallbackTests;native-test-case"
	"target fallback case labels"
)
assert_list_equals(
	"${fallback_target_labels}"
	"native-test;FallbackTests;native-test-target;native-test-direct;native-test-default"
	"target fallback direct labels"
)

durin_resolve_native_test_execution_policy(
	target_default_case_labels
	target_default_target_labels
	QualifiedTests
	TRUE
	TARGET
	""
	""
	LABELS native-test QualifiedTests integration
)
assert_list_equals(
	"${target_default_case_labels}"
	"native-test;QualifiedTests;integration;native-test-case"
	"target-default discovered labels"
)
assert_list_equals(
	"${target_default_target_labels}"
	"native-test;QualifiedTests;integration;native-test-target;native-test-direct;native-test-default"
	"target-default direct labels"
)

durin_resolve_native_test_execution_policy(
	characterization_case_labels
	characterization_target_labels
	CharacterizationTests
	FALSE
	""
	""
	""
	LABELS native-test CharacterizationTests native-test-characterization
)
assert_list_equals(
	"${characterization_case_labels}"
	"native-test;CharacterizationTests;native-test-characterization;native-test-case"
	"characterization discovered labels"
)
assert_list_equals(
	"${characterization_target_labels}"
	""
	"characterization direct labels"
)

assert_policy_rejected("unknown-resource" "unregistered native-test resource")
assert_policy_rejected("broad-lock-without-rationale" "TARGET_LOCK_RATIONALE")
assert_policy_rejected("execution-unknown-granularity" "must be CASE or TARGET")
assert_policy_rejected("execution-case-missing-rationale" "CASE_MIGRATION_RATIONALE")
assert_policy_rejected("execution-case-missing-stage" "CASE_REPAIR_STAGE")
assert_policy_rejected("execution-case-invalid-stage" "CASE_REPAIR_STAGE")
assert_policy_rejected("execution-target-with-case-metadata" "cannot retain CASE migration metadata")
assert_policy_rejected("execution-ordinary-without-direct" "require a direct lifecycle")
assert_policy_rejected("execution-characterization-with-default" "cannot declare ordinary default")
assert_policy_rejected("migration-new-legacy-target" "not grandfathered for legacy")
assert_policy_rejected("repository-retired-work" "retired DURIN_TEST_WORK_DIR")
assert_policy_rejected("repository-direct-remove-all" "RemoveTestWorkDirectory")
assert_policy_rejected("repository-data-write" "mutate checked-in test Data")
assert_policy_rejected("repository-direct-discovery" "registers GoogleTest cases directly")
assert_policy_rejected(
	"repository-post-build-runtime-copy"
	"target-owned POST_BUILD runtime copy"
)

configure_metadata_probe("valid" TRUE "")
configure_metadata_probe("qualification" TRUE "\"kind\":\"qualification\"")
configure_metadata_probe("unavailable" TRUE "")
configure_metadata_probe("missing-kind" FALSE "requires KIND")
configure_metadata_probe("missing-domain" FALSE "requires DOMAINS")
configure_metadata_probe("invalid-kind" FALSE "KIND 'smoke' is invalid")
configure_metadata_probe("invalid-value" FALSE "value 'World_Rendering' is invalid")
configure_metadata_probe("duplicate" FALSE "duplicate value 'world'")
configure_metadata_probe("reserved-label" FALSE "reserved native-test prefix")
configure_metadata_probe("characterization-direct" FALSE "KIND characterization requires")
configure_metadata_probe("private-source" FALSE "compiles production-private source")
