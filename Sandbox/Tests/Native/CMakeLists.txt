include(GoogleTest)

# DurinDevTool and the documented native-test layout use the Engine-wide test bin.
set(DURIN_PROJECT_TEST_OUTPUT_ROOT
	"${CMAKE_SOURCE_DIR}/Engine/Binaries/${DURIN_ARCH}/${DURIN_PROJECT_OUTPUT_CONFIG}/Tests/${DURIN_PROJECT_RUNTIME_VARIANT}")

add_durin_test(SandboxGameplayTests
	Private/SandboxGameplayTests.cpp
)
set_target_properties(SandboxGameplayTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_set_native_test_target_execution(SandboxGameplayTests)
target_link_libraries(SandboxGameplayTests PRIVATE
	Core
	CoreDObject
	Engine
	Sandbox
)
durin_discover_tests(SandboxGameplayTests)
