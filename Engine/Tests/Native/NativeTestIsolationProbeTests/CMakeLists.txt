durin_add_native_test(NativeTestIsolationProbeTests
	KIND infrastructure
	DOMAINS native-test-harness
	SOURCES Private/NativeTestProcessIsolationProbeTests.cpp
	LIBRARIES Core
	INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../NativeTestSupport/Private"
)

durin_add_native_test(NativeTestIsolationCharacterizationTests
	KIND characterization
	DOMAINS native-test-harness
	SOURCES Private/NativeTestProcessIsolationCharacterizationTests.cpp
	LIBRARIES Core
)

get_target_property(
	_durin_native_test_isolation_probe_work_dir
	NativeTestIsolationCharacterizationTests
	DURIN_TEST_WORK_DIR
)

add_custom_target(NativeTestIsolationProbeCharacterization
	COMMAND ${CMAKE_COMMAND}
		"-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
		"-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
		"-DPROBE_WORK_DIRECTORY=${_durin_native_test_isolation_probe_work_dir}"
		"-DPROBE_EXECUTABLE=$<TARGET_FILE:NativeTestIsolationCharacterizationTests>"
		"-DSANDBOX_EXECUTABLE=$<TARGET_FILE:NativeTestIsolationProbeTests>"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyNativeTestIsolationProbe.cmake"
	DEPENDS
		NativeTestIsolationProbeTests
		NativeTestIsolationCharacterizationTests
	COMMENT "Verifying shared-root collision and isolated-root control"
	VERBATIM
)

set_target_properties(NativeTestIsolationProbeCharacterization PROPERTIES
	FOLDER "Tests/NativeTestIsolationProbeTests"
)
