add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../NativeTestSupport"
	"${CMAKE_CURRENT_BINARY_DIR}/NativeTestSupport")

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/CoreTests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/CoreDObjectTests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/AssetCoreTests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/RHITests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/RenderCoreTests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/NativeTestIsolationProbeTests)
if(APPLE AND DURIN_ENABLE_APPLICATION_TESTS)
	add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/NativeTestApplicationHostTests)
	add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/NativeTestApplicationExecutionTests)
else()
	durin_exclude_native_test_sources(
		RATIONALE
			"LaunchServices application-host infrastructure is available only on macOS and runs only in an explicitly enabled validation configuration."
		SOURCES
			${CMAKE_CURRENT_LIST_DIR}/NativeTestApplicationHostTests/Private/NativeTestApplicationProtocolTests.cpp
			${CMAKE_CURRENT_LIST_DIR}/NativeTestApplicationHostTests/Private/NativeTestApplicationControllerTests.cpp
			${CMAKE_CURRENT_LIST_DIR}/NativeTestApplicationExecutionTests/Private/NativeTestApplicationExecutionTests.cpp
	)
endif()
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/VulkanRHITests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/EngineTests)

add_test(
	NAME Durin.PlatformSourceSelection
	COMMAND ${CMAKE_COMMAND}
		"-DDURIN_WORKSPACE_DIR=${DURIN_WORKSPACE_DIR}"
		-P "${DURIN_WORKSPACE_DIR}/CMake/Tests/PlatformSourceSelectionTests.cmake"
)
set_tests_properties(Durin.PlatformSourceSelection PROPERTIES
	LABELS "build;infrastructure;native-test-default"
	TIMEOUT 30
)

add_test(
	NAME Durin.MonaModuleBoundary
	COMMAND ${CMAKE_COMMAND}
		"-DDURIN_WORKSPACE_DIR=${DURIN_WORKSPACE_DIR}"
		-P "${DURIN_WORKSPACE_DIR}/CMake/Tests/MonaModuleBoundaryTests.cmake"
)
set_tests_properties(Durin.MonaModuleBoundary PROPERTIES
	LABELS "build;infrastructure;module-boundary;native-test-default"
	TIMEOUT 30
)

durin_validate_native_test_source_ownership("${CMAKE_CURRENT_LIST_DIR}")
durin_validate_native_test_repository_policy("${CMAKE_CURRENT_LIST_DIR}")

add_test(
	NAME Durin.NativeTestDiscoveryPolicy
	COMMAND ${CMAKE_COMMAND}
		"-DDURIN_WORKSPACE_DIR=${DURIN_WORKSPACE_DIR}"
		"-DDURIN_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
		"-DDURIN_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
		"-DDURIN_CMAKE_GENERATOR=${CMAKE_GENERATOR}"
		"-DDURIN_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
		-P "${DURIN_WORKSPACE_DIR}/CMake/Tests/NativeTestDiscoveryPolicyTests.cmake"
)
set_tests_properties(Durin.NativeTestDiscoveryPolicy PROPERTIES
	LABELS "native-test;infrastructure;native-test-target;native-test-default"
	TIMEOUT 120
)

add_test(
	NAME Durin.NativeTestRuntimeClosure
	COMMAND ${CMAKE_COMMAND}
		"-DDURIN_WORKSPACE_DIR=${DURIN_WORKSPACE_DIR}"
		"-DDURIN_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
		"-DDURIN_CMAKE_GENERATOR=${CMAKE_GENERATOR}"
		"-DDURIN_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
		"-DDURIN_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
		-P "${DURIN_WORKSPACE_DIR}/CMake/Tests/NativeTestRuntimeClosureTests.cmake"
)
set_tests_properties(Durin.NativeTestRuntimeClosure PROPERTIES
	LABELS "native-test;infrastructure;native-test-target;native-test-default"
	TIMEOUT 120
)
