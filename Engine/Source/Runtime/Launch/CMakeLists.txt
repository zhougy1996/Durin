add_library(LaunchPrivateContracts STATIC
	Internal/LaunchContracts/EngineFramePhases.h
	Internal/LaunchContracts/LaunchArguments.cpp
	Internal/LaunchContracts/LaunchArguments.h
	Internal/LaunchContracts/RuntimeStorage.cpp
	Internal/LaunchContracts/RuntimeStorage.h
	Internal/LaunchContracts/WindowsProcessCrashPolicy.cpp
	Internal/LaunchContracts/WindowsProcessCrashPolicy.h
)
durin_target_apply_common_definitions(
	LaunchPrivateContracts
	LaunchPrivateContracts
)
target_include_directories(LaunchPrivateContracts PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/Internal
)
target_link_libraries(LaunchPrivateContracts PUBLIC Core)
target_precompile_headers(LaunchPrivateContracts REUSE_FROM SharedPCH_Core)
set_target_properties(LaunchPrivateContracts PROPERTIES
	FOLDER "Engine/Launch"
	POSITION_INDEPENDENT_CODE TRUE
)

add_durin_module(Launch)
target_link_libraries(Launch PRIVATE LaunchPrivateContracts)

if(WIN32)
	target_link_libraries(Launch PRIVATE Dbghelp)
endif()
