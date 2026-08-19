include(GoogleTest)

# DurinDevTool and the documented native-test layout use the Engine-wide test bin.
set(DURIN_PROJECT_TEST_OUTPUT_ROOT
	"${CMAKE_SOURCE_DIR}/Engine/Binaries/${DURIN_ARCH}/${DURIN_PROJECT_OUTPUT_CONFIG}/Tests/${DURIN_PROJECT_RUNTIME_VARIANT}")

add_durin_test(SandboxGameplayTests
	Private/SandboxGameplayTests.cpp
)
target_link_libraries(SandboxGameplayTests PRIVATE
	Core
	CoreDObject
	Engine
	Sandbox
)
durin_register_native_test(SandboxGameplayTests
	KIND feature
	DOMAINS gameplay
	MODULES engine sandbox
)
