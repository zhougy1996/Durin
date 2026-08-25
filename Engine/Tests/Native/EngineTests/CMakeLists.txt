include(GoogleTest)

set(_durin_level_editor_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private")
set(_durin_material_editor_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Private")
set(_durin_static_mesh_editor_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/StaticMeshEditor/Private")
set(_durin_skeletal_mesh_editor_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/SkeletalMeshEditor/Private")
set(_durin_main_frame_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MainFrame/Private")
set(_durin_content_browser_public
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/ContentBrowser/Public")
set(_durin_editor_test_modules
	AssetForge
	DurinEd
	Mona
	MonaImGui
	GeometryBuild
	TerrainBuild
	DerivedDataCache
	TextureBuild
	AssetForgeBuiltins
)

function(durin_add_engine_functional_test target_name)
	set(options EDITOR_ONLY GPU)
	set(one_value_args
		EXECUTION_HOST
		KIND
		TIMEOUT
		RUNTIME_STACK_RATIONALE
		RUNTIME_ONLY_RATIONALE
		PRIVATE_SOURCE_OWNER
		PRIVATE_SOURCE_RATIONALE
	)
	set(multi_value_args
		DOMAINS
		MODULES
		BACKENDS
		STACKS
		SOURCES
		PRIVATE_SOURCES
		LIBRARIES
		RUNTIME_ONLY_TARGETS
		RUNTIME_ONLY_FILES
		DATA_DIRECTORIES
		INCLUDE_DIRECTORIES
		COMPILE_DEFINITIONS
	)
	cmake_parse_arguments(
		DURIN_ENGINE_TEST
		"${options}"
		"${one_value_args}"
		"${multi_value_args}"
		${ARGN}
	)
	if(APPLE
		AND DURIN_ENGINE_TEST_GPU
		AND vulkan IN_LIST DURIN_ENGINE_TEST_BACKENDS
		AND NOT DURIN_ENGINE_TEST_KIND STREQUAL "qualification"
		AND NOT DURIN_ENGINE_TEST_KIND STREQUAL "characterization")
		# M2 establishes the Cocoa/surface handoff but does not qualify MoltenVK
		# device or rendering behavior. Keep hardware-backed Vulkan targets built
		# and discoverable while excluding them from the ordinary macOS aggregate.
		set(DURIN_ENGINE_TEST_KIND qualification)
	endif()

	if(NOT DURIN_WITH_EDITOR)
		set(_durin_editor_only ${DURIN_ENGINE_TEST_EDITOR_ONLY})
		foreach(_durin_library IN LISTS DURIN_ENGINE_TEST_LIBRARIES)
			if(_durin_library IN_LIST _durin_editor_test_modules)
				set(_durin_editor_only TRUE)
				break()
			endif()
		endforeach()
		foreach(_durin_private_source IN LISTS DURIN_ENGINE_TEST_PRIVATE_SOURCES)
			cmake_path(CONVERT "${_durin_private_source}" TO_CMAKE_PATH_LIST
				_durin_private_source_normalized)
			if(_durin_private_source_normalized MATCHES "/Source/Editor/")
				set(_durin_editor_only TRUE)
				break()
			endif()
		endforeach()
		if(_durin_editor_only)
			durin_exclude_native_test_sources(
				RATIONALE "${DURIN_ENGINE_TEST_RUNTIME_STACK_RATIONALE}"
				SOURCES ${DURIN_ENGINE_TEST_SOURCES}
			)
			return()
		endif()
	endif()

	add_durin_test(${target_name}
		${DURIN_ENGINE_TEST_SOURCES}
		${DURIN_ENGINE_TEST_PRIVATE_SOURCES}
	)
	if(DURIN_ENGINE_TEST_RUNTIME_STACK_RATIONALE)
		set_target_properties(${target_name} PROPERTIES
			DURIN_TEST_HEAVY_RUNTIME_RATIONALE
				"${DURIN_ENGINE_TEST_RUNTIME_STACK_RATIONALE}"
		)
	endif()
	target_include_directories(${target_name} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${_durin_static_mesh_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/StaticMeshEditor/Public
		${_durin_skeletal_mesh_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/SkeletalMeshEditor/Public
		${_durin_main_frame_private}
		${_durin_content_browser_public}
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source
		${DURIN_ENGINE_TEST_INCLUDE_DIRECTORIES}
	)
	target_compile_definitions(${target_name} PRIVATE
		LEVELEDITOR_EXPORTS
		${DURIN_ENGINE_TEST_COMPILE_DEFINITIONS}
	)
	target_link_libraries(${target_name} PRIVATE
		Core
		CoreDObject
		AssetCore
		Engine
		${DURIN_ENGINE_TEST_LIBRARIES}
	)
	if(DURIN_ENGINE_TEST_GPU)
		set_target_properties(${target_name} PROPERTIES
			DURIN_TEST_RESOURCE_LOCKS durin-gpu
		)
	endif()
	if(DURIN_ENGINE_TEST_TIMEOUT)
		set(_durin_engine_test_timeout
			TIMEOUT "${DURIN_ENGINE_TEST_TIMEOUT}")
	endif()
	set(_durin_engine_test_execution_host)
	if(DURIN_ENGINE_TEST_EXECUTION_HOST)
		set(_durin_engine_test_execution_host
			EXECUTION_HOST "${DURIN_ENGINE_TEST_EXECUTION_HOST}")
	endif()
	if(DURIN_ENGINE_TEST_RUNTIME_ONLY_TARGETS
		OR DURIN_ENGINE_TEST_RUNTIME_ONLY_FILES)
		durin_test_register_runtime_only_dependencies(${target_name}
			RATIONALE "${DURIN_ENGINE_TEST_RUNTIME_ONLY_RATIONALE}"
			TARGETS ${DURIN_ENGINE_TEST_RUNTIME_ONLY_TARGETS}
			FILES ${DURIN_ENGINE_TEST_RUNTIME_ONLY_FILES}
		)
	endif()
	foreach(_durin_data_directory IN LISTS DURIN_ENGINE_TEST_DATA_DIRECTORIES)
		durin_test_deploy_directory_to_data(
			${target_name}
			"${_durin_data_directory}"
		)
	endforeach()
	durin_register_native_test(${target_name}
		KIND ${DURIN_ENGINE_TEST_KIND}
		${_durin_engine_test_execution_host}
		DOMAINS ${DURIN_ENGINE_TEST_DOMAINS}
		MODULES ${DURIN_ENGINE_TEST_MODULES}
		BACKENDS ${DURIN_ENGINE_TEST_BACKENDS}
		STACKS ${DURIN_ENGINE_TEST_STACKS}
		${_durin_engine_test_timeout}
		PRIVATE_SOURCE_OWNER ${DURIN_ENGINE_TEST_PRIVATE_SOURCE_OWNER}
		PRIVATE_SOURCE_RATIONALE
			"${DURIN_ENGINE_TEST_PRIVATE_SOURCE_RATIONALE}"
	)
endfunction()


# Keep domain fragments in registration order. Each target remains declared once,
# and registration stays after its complete metadata and dependency setup.
include("${CMAKE_CURRENT_LIST_DIR}/CMake/AssetAndMaterialTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CMake/TextureAndTerrainTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CMake/LaunchTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CMake/RenderingAndThumbnailTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CMake/WorldPhysicsAndViewportTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CMake/SceneAndEditorTests.cmake")
