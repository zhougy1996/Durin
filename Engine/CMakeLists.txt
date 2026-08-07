include(${CMAKE_CURRENT_SOURCE_DIR}/CMake/EngineSetup.cmake)
add_durin_project(Engine)

if(DURIN_RUNTIME_VARIANT STREQUAL "DurinEditor")
	add_subdirectory(Source/Programs/DurinAssetTool)
	add_executable(EnvironmentLightingBake
		"Source/Programs/EnvironmentLightingBake/Main.cpp")
	durin_target_apply_common_definitions(EnvironmentLightingBake "EnvironmentLightingBake")
	target_link_libraries(EnvironmentLightingBake PRIVATE Engine)
	set_target_properties(EnvironmentLightingBake PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY
			"${CMAKE_CURRENT_SOURCE_DIR}/Binaries/${DURIN_TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/Runtime/${DURIN_RUNTIME_VARIANT}")
endif()
