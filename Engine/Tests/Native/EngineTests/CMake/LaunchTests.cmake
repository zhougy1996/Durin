add_durin_test(LaunchStorageTests
	Private/Launch/RuntimeStorageTests.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Launch/Private/RuntimeStorage.cpp
)
target_include_directories(LaunchStorageTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Source)
target_link_libraries(LaunchStorageTests PRIVATE Core)
set_target_properties(LaunchStorageTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_finalize_native_test(LaunchStorageTests
	KIND contract
	DOMAINS launch
	MODULES launch
	PRIVATE_SOURCE_OWNER Launch
	PRIVATE_SOURCE_RATIONALE
		"Launch-owned runtime-storage white-box coverage without exporting private DLL symbols."
)
durin_discover_tests(LaunchStorageTests)

add_durin_test(LaunchArgumentTests
	Private/Launch/LaunchArgumentsTests.cpp
	Private/Launch/EngineFramePhaseTests.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Launch/Private/LaunchArguments.cpp
)
target_include_directories(LaunchArgumentTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Source)
target_link_libraries(LaunchArgumentTests PRIVATE Core)
set_target_properties(LaunchArgumentTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_finalize_native_test(LaunchArgumentTests
	KIND contract
	DOMAINS launch
	MODULES launch
	PRIVATE_SOURCE_OWNER Launch
	PRIVATE_SOURCE_RATIONALE
		"Launch-owned argument parser white-box coverage without exporting private DLL symbols."
)
durin_discover_tests(LaunchArgumentTests)

add_durin_test(NativeWindowModalLoopTests
	Private/Application/NativeWindowModalLoopTests.cpp
)
target_include_directories(NativeWindowModalLoopTests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Source
)
target_link_libraries(NativeWindowModalLoopTests PRIVATE
	Core
	ApplicationCore
)
set_target_properties(NativeWindowModalLoopTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_finalize_native_test(NativeWindowModalLoopTests
	KIND integration
	DOMAINS window
	MODULES application-core
	STACKS native-window
)
durin_discover_tests(NativeWindowModalLoopTests)

add_durin_test(FileDialogContractTests
	Private/Launch/FileDialogContractTests.cpp
)
target_link_libraries(FileDialogContractTests PRIVATE ApplicationCore)
set_target_properties(FileDialogContractTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_finalize_native_test(FileDialogContractTests
	KIND contract
	DOMAINS launch
	MODULES application-core
)
durin_discover_tests(FileDialogContractTests)

add_durin_test(LaunchProcessBoundaryTests
	Private/Launch/LaunchProcessBoundaryTests.cpp
)
add_dependencies(LaunchProcessBoundaryTests DurinLauncher)
target_compile_definitions(LaunchProcessBoundaryTests PRIVATE
	DURIN_LAUNCH_EXECUTABLE="$<TARGET_FILE:DurinLauncher>"
	DURIN_LAUNCH_TEST_PROJECT="${DURIN_WORKSPACE_DIR}/Sandbox/Sandbox.dproject"
)
set_target_properties(LaunchProcessBoundaryTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE FALSE
	DURIN_TEST_HEAVY_RUNTIME_RATIONALE
		"Launches isolated runtime children to verify process exit and cleanup policy."
	DURIN_TEST_RESOURCE_LOCKS durin-gpu
	DURIN_TEST_TARGET_LOCK_RATIONALE
		"Serializes child processes that share the configured launcher binary."
	DURIN_TEST_TIMEOUT 90
)
durin_finalize_native_test(LaunchProcessBoundaryTests
	KIND integration
	DOMAINS launch
	MODULES launch
	STACKS process
)
durin_discover_tests(LaunchProcessBoundaryTests)

if(NOT APPLE)
	durin_exclude_native_test_sources(
		RATIONALE
			"macOS crash-handler and Cocoa window lifecycle coverage requires Apple platform APIs."
		SOURCES
			Private/Launch/MacOSNativeCrashCharacterizationTests.cpp
			Private/Launch/MacOSProcessCrashHandlerTests.cpp
			Private/Launch/MacOSWindowLifecycleTests.cpp
	)
endif()

if(WIN32)
	add_durin_test(NativeCrashCharacterizationTests
		Private/Launch/NativeCrashCharacterizationTests.cpp
		Private/Launch/WindowsProcessCrashPolicyTests.cpp
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Launch/Private/Windows/WindowsProcessCrashPolicy.cpp
	)
	target_include_directories(NativeCrashCharacterizationTests PRIVATE
		${CMAKE_SOURCE_DIR}/Engine/Source)
	target_link_libraries(NativeCrashCharacterizationTests PRIVATE Core)
	add_dependencies(NativeCrashCharacterizationTests DurinLauncher)
	target_compile_definitions(NativeCrashCharacterizationTests PRIVATE
		DURIN_CRASH_FIXTURE_EXECUTABLE="$<TARGET_FILE:DurinLauncher>"
	)
	set_target_properties(NativeCrashCharacterizationTests PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE FALSE
		DURIN_TEST_DIRECT_LIFECYCLE FALSE
		DURIN_TEST_HEAVY_RUNTIME_RATIONALE
			"Launches isolated runtime children because native faults must preserve process exception state and cannot be characterized in-process."
		DURIN_TEST_TARGET_LOCK_RATIONALE
			"Serializes native-fault children and crash-artifact retention checks."
		DURIN_TEST_TIMEOUT 120
	)
	durin_finalize_native_test(NativeCrashCharacterizationTests
		KIND characterization
		DOMAINS launch
		MODULES launch
		STACKS process
		PRIVATE_SOURCE_OWNER Launch
		PRIVATE_SOURCE_RATIONALE
			"Launch-owned crash-policy white-box coverage accompanies isolated native-fault characterization."
	)
	durin_discover_tests(NativeCrashCharacterizationTests)
	elseif(APPLE)
	durin_exclude_native_test_sources(
		RATIONALE
			"Windows native crash characterization and retention policy use Win32 process and dump APIs."
		SOURCES
			Private/Launch/NativeCrashCharacterizationTests.cpp
			Private/Launch/WindowsProcessCrashPolicyTests.cpp
	)

	add_durin_test(MacOSProcessCrashHandlerTests
		Private/Launch/MacOSProcessCrashHandlerTests.cpp
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Launch/Private/MacOS/MacOSProcessCrashHandler.cpp
	)
	target_include_directories(MacOSProcessCrashHandlerTests PRIVATE
		${CMAKE_SOURCE_DIR}/Engine/Source)
	target_link_libraries(MacOSProcessCrashHandlerTests PRIVATE Core)
	set_target_properties(MacOSProcessCrashHandlerTests PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE FALSE
		DURIN_TEST_TARGET_LOCK_RATIONALE
			"Temporarily installs process-wide POSIX signal and terminate handlers."
	)
	durin_finalize_native_test(MacOSProcessCrashHandlerTests
		KIND contract
		DOMAINS launch
		MODULES launch
		STACKS process
		PRIVATE_SOURCE_OWNER Launch
		PRIVATE_SOURCE_RATIONALE
			"Launch-owned macOS crash-handler lifecycle coverage avoids exporting private adapter symbols."
	)
	durin_discover_tests(MacOSProcessCrashHandlerTests)

	add_durin_test(MacOSNativeCrashCharacterizationTests
		Private/Launch/MacOSNativeCrashCharacterizationTests.cpp
	)
	add_dependencies(MacOSNativeCrashCharacterizationTests DurinLauncher)
	target_compile_definitions(MacOSNativeCrashCharacterizationTests PRIVATE
		DURIN_CRASH_FIXTURE_EXECUTABLE="$<TARGET_FILE:DurinLauncher>"
	)
	set_target_properties(MacOSNativeCrashCharacterizationTests PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE FALSE
		DURIN_TEST_DIRECT_LIFECYCLE FALSE
		DURIN_TEST_HEAVY_RUNTIME_RATIONALE
			"Launches isolated runtime children because native POSIX faults cannot be characterized in-process."
		DURIN_TEST_TARGET_LOCK_RATIONALE
			"Serializes native-fault children and crash-artifact retention checks."
		DURIN_TEST_TIMEOUT 120
	)
	durin_finalize_native_test(MacOSNativeCrashCharacterizationTests
		KIND characterization
		DOMAINS launch
		MODULES launch
		STACKS process
	)
	durin_discover_tests(MacOSNativeCrashCharacterizationTests)

	add_durin_test(MacOSWindowLifecycleTests
		Private/Launch/MacOSWindowLifecycleTests.cpp
	)
	target_include_directories(MacOSWindowLifecycleTests PRIVATE
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/ApplicationCore/Private
	)
	target_link_libraries(MacOSWindowLifecycleTests PRIVATE
		ApplicationCore
		"-framework Cocoa"
	)
	set_target_properties(MacOSWindowLifecycleTests PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE FALSE
		DURIN_TEST_HEAVY_RUNTIME_RATIONALE
			"Creates real hidden Cocoa windows and exercises the host monitor and event services."
		DURIN_TEST_TARGET_LOCK_RATIONALE
			"Serializes access to the process-global GLFW Cocoa lifecycle."
	)
	durin_finalize_native_test(MacOSWindowLifecycleTests
		KIND qualification
		EXECUTION_HOST application
		DOMAINS launch
		MODULES application-core
		STACKS window
	)
	durin_discover_tests(MacOSWindowLifecycleTests)
	else()
	durin_exclude_native_test_sources(
		RATIONALE
			"Native crash characterization currently requires the Windows crash-policy fixture."
		SOURCES
			Private/Launch/NativeCrashCharacterizationTests.cpp
			Private/Launch/WindowsProcessCrashPolicyTests.cpp
	)
endif()
