include(GoogleTest)

add_durin_test(NativeTestIsolationProbeTests
	Private/NativeTestProcessIsolationProbeTests.cpp
)

target_include_directories(NativeTestIsolationProbeTests PRIVATE
	"${CMAKE_CURRENT_LIST_DIR}/../../NativeTestSupport/Private"
)

target_link_libraries(NativeTestIsolationProbeTests PRIVATE
	Core
)

durin_test_deploy_target_binary(NativeTestIsolationProbeTests Core)

set_target_properties(NativeTestIsolationProbeTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
	DURIN_TEST_LABELS native-test-isolation-characterization
)

# The cases skip in routine runs. The characterization target supplies a shared
# synchronization directory and proves that two CTest processes retain distinct
# writable roots while using the same logical filename.
durin_discover_tests(NativeTestIsolationProbeTests)

get_target_property(
	_durin_native_test_isolation_probe_work_dir
	NativeTestIsolationProbeTests
	DURIN_TEST_WORK_DIR
)

add_custom_target(NativeTestIsolationProbeCharacterization
	COMMAND ${CMAKE_COMMAND}
		"-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
		"-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
		"-DPROBE_WORK_DIRECTORY=${_durin_native_test_isolation_probe_work_dir}"
		"-DPROBE_EXECUTABLE=$<TARGET_FILE:NativeTestIsolationProbeTests>"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyNativeTestIsolationProbe.cmake"
	DEPENDS NativeTestIsolationProbeTests
	COMMENT "Verifying shared-root collision and isolated-root control"
	VERBATIM
)

set_target_properties(NativeTestIsolationProbeCharacterization PROPERTIES
	FOLDER "Tests/NativeTestIsolationProbeTests"
)
