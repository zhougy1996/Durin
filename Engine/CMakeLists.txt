include(${CMAKE_CURRENT_SOURCE_DIR}/CMake/EngineSetup.cmake)
add_durin_project(Engine)
add_subdirectory(Source/Programs/DurinAssetAudit)

if(DURIN_RUNTIME_VARIANT STREQUAL "DurinEditor")
	add_executable(EnvironmentLightingBake
		"Source/Programs/EnvironmentLightingBake/Main.cpp")
	target_link_libraries(EnvironmentLightingBake PRIVATE Engine)
	set_target_properties(EnvironmentLightingBake PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY
			"${CMAKE_CURRENT_SOURCE_DIR}/Binaries/${DURIN_TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/Runtime/${DURIN_RUNTIME_VARIANT}")
endif()
