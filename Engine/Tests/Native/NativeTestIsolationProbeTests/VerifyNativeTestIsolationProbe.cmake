if(NOT DEFINED CTEST_COMMAND OR NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED PROBE_WORK_DIRECTORY)
	message(FATAL_ERROR "The isolation probe requires CTest, build, and work paths.")
endif()

function(run_probe mode expected_result)
	file(REMOVE_RECURSE
		"${PROBE_WORK_DIRECTORY}/ProcessIsolationProbe"
		"${PROBE_WORK_DIRECTORY}/ProcessIsolationProbeControl"
	)

	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
			"DURIN_TEST_ISOLATION_PROBE_MODE=${mode}"
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

	message(STATUS "Isolation probe mode '${mode}':\n${probe_output}${probe_error}")

	if(expected_result STREQUAL "failure" AND probe_result EQUAL 0)
		message(FATAL_ERROR "The legacy shared-root probe unexpectedly passed.")
	elseif(expected_result STREQUAL "success" AND NOT probe_result EQUAL 0)
		message(FATAL_ERROR "The isolated-root control failed with exit code ${probe_result}.")
	endif()
endfunction()

run_probe(legacy failure)
run_probe(isolated success)
