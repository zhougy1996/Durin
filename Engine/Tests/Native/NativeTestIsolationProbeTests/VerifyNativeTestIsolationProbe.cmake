if(NOT DEFINED CTEST_COMMAND OR NOT DEFINED BUILD_DIRECTORY
	OR NOT DEFINED PROBE_WORK_DIRECTORY OR NOT DEFINED PROBE_EXECUTABLE
	OR NOT DEFINED SANDBOX_EXECUTABLE)
	message(FATAL_ERROR
		"The isolation probe requires CTest, build, work, probe, and sandbox executable paths.")
endif()

set(control_directory "${PROBE_WORK_DIRECTORY}/ProcessIsolationProbeControl")
file(REMOVE_RECURSE "${control_directory}")

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
		"DURIN_TEST_ISOLATION_PROBE_CONTROL=${control_directory}"
		"${CTEST_COMMAND}"
		--test-dir "${BUILD_DIRECTORY}"
		--output-on-failure
		--no-tests=error
		-j 2
		-R "^FNativeTestProcessIsolationProbeTests\\."
	RESULT_VARIABLE probe_result
	OUTPUT_VARIABLE probe_output
	ERROR_VARIABLE probe_error
)
message(STATUS "Process sandbox concurrency probe:\n${probe_output}${probe_error}")
if(NOT probe_result EQUAL 0)
	message(FATAL_ERROR "The process sandbox concurrency probe failed with exit code ${probe_result}.")
endif()

execute_process(
	COMMAND "${SANDBOX_EXECUTABLE}"
		--gtest_filter=FNativeTestProcessSandboxTests.ProvidesCanonicalUniqueRunDirectory
		--durin-keep-test-work
	RESULT_VARIABLE keep_result
	OUTPUT_VARIABLE keep_output
	ERROR_VARIABLE keep_error
)
message(STATUS "Keep-work probe:\n${keep_output}${keep_error}")
if(NOT keep_result EQUAL 0)
	message(FATAL_ERROR "The keep-work probe failed with exit code ${keep_result}.")
endif()
string(REGEX MATCH "Preserved test work directory: ([^\r\n]+)" keep_match "${keep_output}")
set(kept_directory "${CMAKE_MATCH_1}")
if(NOT kept_directory OR NOT IS_DIRECTORY "${kept_directory}")
	message(FATAL_ERROR "The keep-work probe did not retain its reported sandbox.")
endif()
file(REMOVE_RECURSE "${kept_directory}")

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env
		"DURIN_TEST_FORCE_CLEANUP_FAILURE=1"
		"${SANDBOX_EXECUTABLE}"
		--gtest_filter=FNativeTestProcessSandboxTests.ProvidesCanonicalUniqueRunDirectory
	RESULT_VARIABLE cleanup_result
	OUTPUT_VARIABLE cleanup_output
	ERROR_VARIABLE cleanup_error
)
message(STATUS "Cleanup-failure probe:\n${cleanup_output}${cleanup_error}")
if(NOT cleanup_result EQUAL 0)
	message(FATAL_ERROR "The cleanup-failure probe test failed with exit code ${cleanup_result}.")
endif()
string(REGEX MATCH "Test work directory: ([^\r\n]+)" cleanup_match "${cleanup_output}")
set(cleanup_directory "${CMAKE_MATCH_1}")
if(NOT cleanup_directory OR NOT IS_DIRECTORY "${cleanup_directory}"
	OR NOT cleanup_error MATCHES "forced cleanup failure")
	message(FATAL_ERROR "The cleanup-failure probe did not report and retain its sandbox.")
endif()
file(REMOVE_RECURSE "${cleanup_directory}")

execute_process(
	COMMAND "${SANDBOX_EXECUTABLE}" --durin-crash-after-sandbox-create
	RESULT_VARIABLE crash_result
	OUTPUT_VARIABLE crash_output
	ERROR_VARIABLE crash_error
)
message(STATUS "Crash-retention probe:\n${crash_output}${crash_error}")
if(crash_result EQUAL 0)
	message(FATAL_ERROR "The crash-retention probe unexpectedly exited successfully.")
endif()
string(REGEX MATCH "Test work directory: ([^\r\n]+)" crash_match "${crash_output}")
set(crash_directory "${CMAKE_MATCH_1}")
if(NOT crash_directory OR NOT IS_DIRECTORY "${crash_directory}")
	message(FATAL_ERROR "The crash-retention probe did not retain its reported sandbox.")
endif()
file(REMOVE_RECURSE "${crash_directory}")
