include(GoogleTest)

add_durin_test(NativeTestIsolationProbeTests
	Private/NativeTestProcessIsolationProbeTests.cpp
)

target_link_libraries(NativeTestIsolationProbeTests PRIVATE
	Core
	GTest::gtest_main
)

durin_test_deploy_target_binary(NativeTestIsolationProbeTests Core)

get_target_property(
	_durin_native_test_isolation_probe_work_dir
	NativeTestIsolationProbeTests
	DURIN_TEST_WORK_DIR
)

# Stage 0 characterization: the cases skip in routine runs. Set
# DURIN_TEST_ISOLATION_PROBE_MODE to "legacy" or "isolated" to exercise the two
# expected outcomes. Stage 3 removes that switch and runs against the process
# harness sandbox.
gtest_discover_tests(NativeTestIsolationProbeTests
	WORKING_DIRECTORY "${_durin_native_test_isolation_probe_work_dir}"
	PROPERTIES
		LABELS "native-test-isolation-characterization"
)

add_custom_target(NativeTestIsolationProbeCharacterization
	COMMAND ${CMAKE_COMMAND}
		"-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
		"-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
		"-DPROBE_WORK_DIRECTORY=${_durin_native_test_isolation_probe_work_dir}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyNativeTestIsolationProbe.cmake"
	DEPENDS NativeTestIsolationProbeTests
	COMMENT "Verifying shared-root collision and isolated-root control"
	VERBATIM
)

set_target_properties(NativeTestIsolationProbeCharacterization PROPERTIES
	FOLDER "Tests/NativeTestIsolationProbeTests"
)
