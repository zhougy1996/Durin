include(GoogleTest)

add_durin_test(NativeTestIsolationProbeTests
	Private/NativeTestProcessIsolationProbeTests.cpp
)

add_durin_test(NativeTestIsolationCharacterizationTests
	Private/NativeTestProcessIsolationCharacterizationTests.cpp
)

set_target_properties(NativeTestIsolationCharacterizationTests PROPERTIES
	DURIN_TEST_DIRECT_LIFECYCLE FALSE
)

target_include_directories(NativeTestIsolationProbeTests PRIVATE
	"${CMAKE_CURRENT_LIST_DIR}/../../NativeTestSupport/Private"
)

foreach(_durin_isolation_target IN ITEMS
	NativeTestIsolationProbeTests
	NativeTestIsolationCharacterizationTests
)
	target_link_libraries(${_durin_isolation_target} PRIVATE Core)
	set_target_properties(${_durin_isolation_target} PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
	)
	if(_durin_isolation_target STREQUAL "NativeTestIsolationCharacterizationTests")
		durin_finalize_native_test(${_durin_isolation_target}
			KIND characterization
			DOMAINS native-test-harness
		)
	else()
		durin_finalize_native_test(${_durin_isolation_target}
			KIND infrastructure
			DOMAINS native-test-harness
		)
	endif()
	durin_discover_tests(${_durin_isolation_target})
endforeach()

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
