cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED DURIN_WORKSPACE_DIR)
	message(FATAL_ERROR "DURIN_WORKSPACE_DIR is required.")
endif()
if(NOT DEFINED DURIN_CMAKE_GENERATOR OR NOT DEFINED DURIN_CXX_COMPILER)
	message(FATAL_ERROR "DURIN_CMAKE_GENERATOR and DURIN_CXX_COMPILER are required.")
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
	set(_durin_application_tests OFF)
	if(probe STREQUAL "application-host")
		set(_durin_application_tests ON)
	endif()
	execute_process(
		COMMAND "${CMAKE_COMMAND}"
			-G Ninja
			-Werror=dev
			"-DCMAKE_MAKE_PROGRAM=${DURIN_MAKE_PROGRAM}"
			-S "${DURIN_WORKSPACE_DIR}/CMake/Tests/Fixtures/NativeTestMetadata"
			-B "${_durin_probe_binary}"
				"-DDURIN_WORKSPACE_DIR=${DURIN_WORKSPACE_DIR}"
				"-DDURIN_METADATA_PROBE=${probe}"
				"-DDURIN_ENABLE_APPLICATION_TESTS=${_durin_application_tests}"
				"-DCMAKE_BUILD_TYPE=Debug"
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
		elseif(NOT _durin_first_registry MATCHES "\"schemaVersion\": 4")
			message(FATAL_ERROR "Metadata probe registry omitted schema version 4.")
		endif()
		if(_durin_first_registry MATCHES
			"\"(directLifecycle|timeoutSeconds)\"")
			message(FATAL_ERROR
				"Metadata probe registry retained unused execution fields.")
		endif()
		if(expected_text AND NOT _durin_first_registry MATCHES "${expected_text}")
			message(FATAL_ERROR
				"Metadata probe '${probe}' registry omitted '${expected_text}':\n"
				"${_durin_first_registry}")
		endif()
		if(NOT probe STREQUAL "unavailable")
			file(READ "${_durin_probe_binary}/ProbeTests-paths.txt" _durin_paths)
			if(_durin_paths MATCHES "\\$<CONFIG>")
				message(FATAL_ERROR
					"Metadata probe '${probe}' generated a literal $<CONFIG> path:\n"
					"${_durin_paths}")
			endif()
			if(probe STREQUAL "application-host")
				if(_durin_paths MATCHES "/private/tmp")
					message(FATAL_ERROR
						"Metadata probe '${probe}' escaped into /private/tmp:\n"
						"${_durin_paths}")
				endif()
				foreach(_durin_expected_path IN ITEMS Bin Data Work)
					if(NOT _durin_paths MATCHES
						"Generated/Debug/ProbeTests/${_durin_expected_path}")
						message(FATAL_ERROR
							"Metadata probe '${probe}' omitted target-local "
							"${_durin_expected_path}:\n${_durin_paths}")
					endif()
				endforeach()
			endif()
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

function(run_test_launcher_probe)
	set(_durin_probe_binary "${DURIN_TEST_BINARY_DIR}/TestLauncherProbe")
	file(REMOVE_RECURSE "${_durin_probe_binary}")
	execute_process(
		COMMAND "${CMAKE_COMMAND}"
			-G "${DURIN_CMAKE_GENERATOR}"
			"-DCMAKE_MAKE_PROGRAM=${DURIN_MAKE_PROGRAM}"
			"-DCMAKE_CXX_COMPILER=${DURIN_CXX_COMPILER}"
			-S "${DURIN_WORKSPACE_DIR}/CMake/Tests/Fixtures/NativeTestLauncher"
			-B "${_durin_probe_binary}"
		RESULT_VARIABLE _durin_configure_result
		OUTPUT_VARIABLE _durin_configure_output
		ERROR_VARIABLE _durin_configure_error
	)
	if(NOT _durin_configure_result EQUAL 0)
		message(FATAL_ERROR
			"TEST_LAUNCHER probe configure failed:\n"
			"${_durin_configure_output}\n${_durin_configure_error}")
	endif()
	execute_process(
		COMMAND "${CMAKE_COMMAND}" --build "${_durin_probe_binary}"
		RESULT_VARIABLE _durin_build_result
		OUTPUT_VARIABLE _durin_build_output
		ERROR_VARIABLE _durin_build_error
	)
	if(NOT _durin_build_result EQUAL 0)
		message(FATAL_ERROR
			"TEST_LAUNCHER probe build failed:\n"
			"${_durin_build_output}\n${_durin_build_error}")
	endif()
	execute_process(
		COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${_durin_probe_binary}"
			--output-on-failure
		RESULT_VARIABLE _durin_ctest_result
		OUTPUT_VARIABLE _durin_ctest_output
		ERROR_VARIABLE _durin_ctest_error
	)
	if(NOT _durin_ctest_result EQUAL 0)
		message(FATAL_ERROR
			"TEST_LAUNCHER probe execution failed:\n"
			"${_durin_ctest_output}\n${_durin_ctest_error}")
	endif()
	file(READ "${_durin_probe_binary}/launcher.log" _durin_launcher_log)
	foreach(_durin_expected IN ITEMS discovery case whole-target)
		if(NOT _durin_launcher_log MATCHES "${_durin_expected}")
			message(FATAL_ERROR
				"TEST_LAUNCHER probe did not observe ${_durin_expected}:\n"
				"${_durin_launcher_log}")
		endif()
	endforeach()
endfunction()

durin_resolve_native_test_discovery_policy(
	default_locks
	default_labels
	CoreUtilityTests
	FALSE
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
	LABELS fast core
)
assert_list_equals("${parallel_locks}" "" "parallel-safe target lock")
assert_list_equals(
	"${parallel_labels}"
	"native-test;CoreConcurrencyTests;fast;core"
	"parallel-safe labels"
)

durin_resolve_native_test_discovery_policy(
	explicit_locks
	explicit_labels
	TextureCookIntegrationTests
	TRUE
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	LABELS integration
)
assert_list_equals(
	"${explicit_locks}"
	"durin-gpu;durin-rhi-lifecycle"
	"explicit resource lock"
)
assert_list_equals(
	"${explicit_labels}"
	"native-test;TextureCookIntegrationTests;integration"
	"explicit-lock labels"
)
assert_list_equals(
	"${DURIN_NATIVE_TEST_RESOURCE_LOCK_REGISTRY}"
	"durin-gpu;durin-rhi-lifecycle"
	"documented explicit resource registry"
)
durin_resolve_native_test_execution_policy(
	fallback_case_labels
	fallback_target_labels
	FallbackTests
	TRUE
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
assert_policy_rejected("execution-ordinary-without-direct" "require a direct lifecycle")
assert_policy_rejected("repository-retired-work" "retired DURIN_TEST_WORK_DIR")
assert_policy_rejected("repository-direct-remove-all" "RemoveTestWorkDirectory")
assert_policy_rejected("repository-data-write" "mutate checked-in test Data")
assert_policy_rejected("repository-direct-discovery" "registers GoogleTest cases directly")
assert_policy_rejected(
	"repository-post-build-runtime-copy"
	"target-owned POST_BUILD runtime copy"
)

configure_metadata_probe("valid" TRUE "\"executionHost\":\"direct\",\"resolvedExecutionHost\":\"direct\"")
configure_metadata_probe("qualification" TRUE "\"kind\":\"qualification\"")
configure_metadata_probe("unavailable" TRUE "")
configure_metadata_probe("missing-kind" FALSE "requires KIND")
configure_metadata_probe("missing-domain" FALSE "requires DOMAINS")
configure_metadata_probe("invalid-kind" FALSE "KIND 'smoke' is invalid")
configure_metadata_probe("invalid-value" FALSE "value 'World_Rendering' is invalid")
configure_metadata_probe("duplicate" FALSE "duplicate value 'world'")
configure_metadata_probe("reserved-label" FALSE "reserved native-test prefix")
configure_metadata_probe("characterization" TRUE "\"kind\":\"characterization\"")
configure_metadata_probe("private-source" FALSE "compiles production-private source")
configure_metadata_probe("generated-main" TRUE "")
configure_metadata_probe("generated-main-private-source" FALSE "compiles production-private source")
if(APPLE)
	configure_metadata_probe("application-host-disabled" FALSE
		"DURIN_ENABLE_APPLICATION_TESTS is OFF")
else()
	configure_metadata_probe("application-host-disabled" TRUE
		"\"executionHost\":\"application\",\"resolvedExecutionHost\":\"direct\"")
endif()
configure_metadata_probe("application-host" TRUE "\"executionHost\":\"application\"")
configure_metadata_probe("property-execution-host" FALSE "is finalized metadata")
configure_metadata_probe("synthetic-platform-resolution" TRUE "\"executionHost\":\"direct\"")
configure_metadata_probe("invalid-execution-host" FALSE "execution host 'gui' is invalid")
configure_metadata_probe("empty-execution-host" FALSE "EXECUTION_HOST must name")
configure_metadata_probe("late-execution-host" FALSE "changed after")
run_test_launcher_probe()
