cmake_minimum_required(VERSION 3.24)

foreach(_durin_required_var IN ITEMS
	DURIN_WORKSPACE_DIR
	DURIN_TEST_BINARY_DIR
	DURIN_CMAKE_GENERATOR
	DURIN_CXX_COMPILER
)
	if(NOT DEFINED ${_durin_required_var})
		message(FATAL_ERROR "${_durin_required_var} is required.")
	endif()
endforeach()

set(_durin_fixture_source
	"${DURIN_WORKSPACE_DIR}/CMake/Tests/Fixtures/NativeTestRuntimeClosure")
set(_durin_fixture_root
	"${DURIN_TEST_BINARY_DIR}/NativeTestRuntimeClosureProbe")

function(run_closure_probe probe expected_success expected_text)
	set(_durin_probe_binary "${_durin_fixture_root}/${probe}")
	file(REMOVE_RECURSE "${_durin_probe_binary}")
	set(_durin_configure_command
		"${CMAKE_COMMAND}"
		-S "${_durin_fixture_source}"
		-B "${_durin_probe_binary}"
		-G "${DURIN_CMAKE_GENERATOR}"
		"-DCMAKE_BUILD_TYPE=Debug"
		"-DCMAKE_CXX_COMPILER=${DURIN_CXX_COMPILER}"
		"-DDURIN_WORKSPACE_DIR=${DURIN_WORKSPACE_DIR}"
		"-DDURIN_CLOSURE_PROBE=${probe}"
	)
	if(DEFINED DURIN_MAKE_PROGRAM AND DURIN_MAKE_PROGRAM)
		list(APPEND _durin_configure_command
			"-DCMAKE_MAKE_PROGRAM=${DURIN_MAKE_PROGRAM}")
	endif()

	execute_process(
		COMMAND ${_durin_configure_command}
		RESULT_VARIABLE _durin_probe_result
		OUTPUT_VARIABLE _durin_probe_output
		ERROR_VARIABLE _durin_probe_error
	)
	set(_durin_probe_text "${_durin_probe_output}\n${_durin_probe_error}")
	if(expected_success)
		if(NOT _durin_probe_result EQUAL 0)
			message(FATAL_ERROR
				"Closure probe '${probe}' failed unexpectedly:\n${_durin_probe_text}")
		endif()
	else()
		if(_durin_probe_result EQUAL 0)
			message(FATAL_ERROR
				"Closure probe '${probe}' unexpectedly succeeded.")
		endif()
		if(NOT _durin_probe_text MATCHES "${expected_text}")
			message(FATAL_ERROR
				"Closure probe '${probe}' did not report '${expected_text}':\n"
				"${_durin_probe_text}")
		endif()
	endif()
endfunction()

run_closure_probe("success" TRUE "")
run_closure_probe(
	"missing-runtime-target" FALSE "missing runtime-only target")
run_closure_probe(
	"linked-runtime-only" FALSE "lists linked target shared_leaf as runtime-only")
run_closure_probe(
	"unknown-expression" FALSE "unsupported target-bearing link expression")
run_closure_probe(
	"destination-collision" FALSE "runtime deployment collision")
run_closure_probe(
	"manual-duplicate" FALSE "manually deploys targets already provided")
run_closure_probe(
	"manual-file-duplicate" FALSE "manually deploys files already provided")
