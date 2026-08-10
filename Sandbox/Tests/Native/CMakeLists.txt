include(GoogleTest)

# DurinDevTool and the documented native-test layout use the Engine-wide test bin.
set(DURIN_PROJECT_TEST_OUTPUT_ROOT
	"${CMAKE_SOURCE_DIR}/Engine/Binaries/${DURIN_ARCH}/${DURIN_PROJECT_OUTPUT_CONFIG}/Tests/${DURIN_PROJECT_RUNTIME_VARIANT}")

add_durin_test(SandboxGameplayReflectionTests
	Private/CrossModuleGameModeTests.cpp
)
set_target_properties(SandboxGameplayReflectionTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_set_native_test_target_execution(SandboxGameplayReflectionTests)
target_link_libraries(SandboxGameplayReflectionTests PRIVATE
	Core
	CoreDObject
	Engine
)
durin_test_register_runtime_only_dependencies(SandboxGameplayReflectionTests
	RATIONALE "Loads the Sandbox qualification fixture to prove reflected game-mode lookup across project-module boundaries."
	TARGETS Sandbox
)
durin_discover_tests(SandboxGameplayReflectionTests)
