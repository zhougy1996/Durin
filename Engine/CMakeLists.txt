include(${CMAKE_CURRENT_SOURCE_DIR}/CMake/EngineSetup.cmake)
add_durin_project(Engine)

if(DURIN_RUNTIME_VARIANT STREQUAL "DurinEditor")
	add_subdirectory(Source/Programs/DurinAssetTool)
	set(DURIN_WITH_EDITOR 1)
	add_executable(StudioCubeGenerate
		"Source/Programs/StudioCubeGenerate/Main.cpp")
	durin_target_apply_common_definitions(StudioCubeGenerate "StudioCubeGenerate")
	target_link_libraries(StudioCubeGenerate PRIVATE Engine)
	add_dependencies(StudioCubeGenerate TextureBuild)
	set_target_properties(StudioCubeGenerate PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY
			"${CMAKE_CURRENT_SOURCE_DIR}/Binaries/${DURIN_TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/Runtime/${DURIN_RUNTIME_VARIANT}")
endif()
