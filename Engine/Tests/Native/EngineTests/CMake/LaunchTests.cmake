durin_add_native_test(LaunchStorageTests
	KIND contract
	DOMAINS launch
	MODULES launch
	PRIVATE_SOURCE_OWNER Launch
	PRIVATE_SOURCE_RATIONALE
		"Launch-owned runtime-storage white-box coverage without exporting private DLL symbols."
	SOURCES
		Private/Launch/RuntimeStorageTests.cpp
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Launch/Private/RuntimeStorage.cpp
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source
	LIBRARIES Core
)

durin_add_native_test(LaunchArgumentTests
	KIND contract
	DOMAINS launch
	MODULES launch
	PRIVATE_SOURCE_OWNER Launch
	PRIVATE_SOURCE_RATIONALE
		"Launch-owned argument parser white-box coverage without exporting private DLL symbols."
	SOURCES
		Private/Launch/LaunchArgumentsTests.cpp
		Private/Launch/EngineFramePhaseTests.cpp
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Launch/Private/LaunchArguments.cpp
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source
	LIBRARIES Core
)

durin_add_native_test(NativeWindowModalLoopTests
	KIND integration
	DOMAINS window
	MODULES application-core
	STACKS native-window
	SOURCES Private/Application/NativeWindowModalLoopTests.cpp
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source
	LIBRARIES Core ApplicationCore
)

durin_add_native_test(FileDialogContractTests
	KIND contract
	DOMAINS launch
	MODULES application-core
	SOURCES Private/Launch/FileDialogContractTests.cpp
	LIBRARIES ApplicationCore
)

durin_add_native_test(LaunchProcessBoundaryTests
	KIND integration
	DOMAINS launch
	MODULES launch
	STACKS process
	TIMEOUT 90
	SERIAL
	SOURCES Private/Launch/LaunchProcessBoundaryTests.cpp
	DEPENDENCIES DurinLauncher
	COMPILE_DEFINITIONS
		DURIN_LAUNCH_EXECUTABLE="$<TARGET_FILE:DurinLauncher>"
		DURIN_LAUNCH_TEST_PROJECT="${DURIN_WORKSPACE_DIR}/Sandbox/Sandbox.dproject"
	HEAVY_RUNTIME_RATIONALE "Launches isolated runtime children to verify process exit and cleanup policy."
	RESOURCE_LOCKS "durin-gpu;durin-rhi-lifecycle"
	TARGET_LOCK_RATIONALE "Serializes child processes that share the configured launcher binary."
)

if(NOT APPLE)
	durin_exclude_native_test_sources(
		RATIONALE
			"macOS crash-handler and Cocoa window lifecycle coverage requires Apple platform APIs."
		SOURCES Private/Launch/MacOSNativeCrashCharacterizationTests.cpp
			Private/Launch/MacOSProcessCrashHandlerTests.cpp
			Private/Launch/MacOSWindowLifecycleTests.cpp
	)
endif()

if(WIN32)
	durin_add_native_test(NativeCrashCharacterizationTests
		KIND characterization
		DOMAINS launch
		MODULES launch
		STACKS process
		PRIVATE_SOURCE_OWNER Launch
		PRIVATE_SOURCE_RATIONALE
			"Launch-owned crash-policy white-box coverage accompanies isolated native-fault characterization."
		TIMEOUT 120
		SERIAL
		SOURCES
			Private/Launch/NativeCrashCharacterizationTests.cpp
			Private/Launch/WindowsProcessCrashPolicyTests.cpp
			${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Launch/Private/Windows/WindowsProcessCrashPolicy.cpp
		INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source
		LIBRARIES Core
		DEPENDENCIES DurinLauncher
		COMPILE_DEFINITIONS DURIN_CRASH_FIXTURE_EXECUTABLE="$<TARGET_FILE:DurinLauncher>"
		HEAVY_RUNTIME_RATIONALE
			"Launches isolated runtime children because native faults must preserve process exception state and cannot be characterized in-process."
		TARGET_LOCK_RATIONALE "Serializes native-fault children and crash-artifact retention checks."
	)
	elseif(APPLE)
	durin_exclude_native_test_sources(
		RATIONALE
			"Windows native crash characterization and retention policy use Win32 process and dump APIs."
		SOURCES Private/Launch/NativeCrashCharacterizationTests.cpp
			Private/Launch/WindowsProcessCrashPolicyTests.cpp
	)

	durin_add_native_test(MacOSProcessCrashHandlerTests
		KIND contract
		DOMAINS launch
		MODULES launch
		STACKS process
		PRIVATE_SOURCE_OWNER Launch
		PRIVATE_SOURCE_RATIONALE
			"Launch-owned macOS crash-handler lifecycle coverage avoids exporting private adapter symbols."
		SERIAL
		SOURCES
			Private/Launch/MacOSProcessCrashHandlerTests.cpp
			${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Launch/Private/MacOS/MacOSProcessCrashHandler.cpp
		INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source
		LIBRARIES Core
		TARGET_LOCK_RATIONALE "Temporarily installs process-wide POSIX signal and terminate handlers."
	)

	durin_add_native_test(MacOSNativeCrashCharacterizationTests
		KIND characterization
		DOMAINS launch
		MODULES launch
		STACKS process
		TIMEOUT 120
		SERIAL
		SOURCES Private/Launch/MacOSNativeCrashCharacterizationTests.cpp
		DEPENDENCIES DurinLauncher
		COMPILE_DEFINITIONS DURIN_CRASH_FIXTURE_EXECUTABLE="$<TARGET_FILE:DurinLauncher>"
		HEAVY_RUNTIME_RATIONALE
			"Launches isolated runtime children because native POSIX faults cannot be characterized in-process."
		TARGET_LOCK_RATIONALE "Serializes native-fault children and crash-artifact retention checks."
	)

	if(DURIN_ENABLE_APPLICATION_TESTS)
		durin_add_native_test(MacOSWindowLifecycleTests
			KIND qualification
			EXECUTION_HOST application
			DOMAINS launch
			MODULES application-core
			STACKS window
			SERIAL
			SOURCES Private/Launch/MacOSWindowLifecycleTests.cpp
			INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/ApplicationCore/Private
			LIBRARIES ApplicationCore "-framework Cocoa"
			HEAVY_RUNTIME_RATIONALE
				"Creates real hidden Cocoa windows and exercises the host monitor and event services."
			TARGET_LOCK_RATIONALE "Serializes access to the process-global GLFW Cocoa lifecycle."
		)
	else()
		durin_exclude_native_test_sources(
			RATIONALE
				"Cocoa window qualification runs only when application tests are explicitly enabled."
			SOURCES Private/Launch/MacOSWindowLifecycleTests.cpp
		)
	endif()
	else()
	durin_exclude_native_test_sources(
		RATIONALE
			"Native crash characterization currently requires the Windows crash-policy fixture."
		SOURCES Private/Launch/NativeCrashCharacterizationTests.cpp
			Private/Launch/WindowsProcessCrashPolicyTests.cpp
	)
endif()
