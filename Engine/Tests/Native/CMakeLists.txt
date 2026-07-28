add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../NativeTestSupport"
	"${CMAKE_CURRENT_BINARY_DIR}/NativeTestSupport")

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/CoreTests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/CoreDObjectTests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/AssetCoreTests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/RenderCoreTests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/NativeTestIsolationProbeTests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/VulkanRHITests)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/EngineTests)

durin_validate_native_test_source_ownership("${CMAKE_CURRENT_LIST_DIR}")
durin_validate_native_test_repository_policy("${CMAKE_CURRENT_LIST_DIR}")

add_test(
	NAME Durin.NativeTestDiscoveryPolicy
	COMMAND ${CMAKE_COMMAND}
		"-DDURIN_WORKSPACE_DIR=${DURIN_WORKSPACE_DIR}"
		"-DDURIN_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
		-P "${DURIN_WORKSPACE_DIR}/CMake/Tests/NativeTestDiscoveryPolicyTests.cmake"
)
set_tests_properties(Durin.NativeTestDiscoveryPolicy PROPERTIES
	LABELS "native-test;infrastructure"
	TIMEOUT 30
)
