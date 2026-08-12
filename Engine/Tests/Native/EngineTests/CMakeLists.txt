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
set(_durin_editor_test_modules
	AssetImportCore
	DurinEd
	Mona
	MonaImGui
	EngineAssetBuild
	StandardAssetImport
)

function(durin_add_engine_functional_test target_name)
	set(options EDITOR_ONLY GPU)
	set(one_value_args
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
	set_target_properties(${target_name} PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
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

	if(DURIN_ENGINE_TEST_GPU)
		set_target_properties(${target_name} PROPERTIES
			DURIN_TEST_RESOURCE_LOCKS durin-gpu
		)
	endif()
	if(DURIN_ENGINE_TEST_TIMEOUT)
		set_target_properties(${target_name} PROPERTIES
			DURIN_TEST_TIMEOUT "${DURIN_ENGINE_TEST_TIMEOUT}"
		)
	endif()
	durin_finalize_native_test(${target_name}
		KIND ${DURIN_ENGINE_TEST_KIND}
		DOMAINS ${DURIN_ENGINE_TEST_DOMAINS}
		MODULES ${DURIN_ENGINE_TEST_MODULES}
		BACKENDS ${DURIN_ENGINE_TEST_BACKENDS}
		STACKS ${DURIN_ENGINE_TEST_STACKS}
		PRIVATE_SOURCE_OWNER ${DURIN_ENGINE_TEST_PRIVATE_SOURCE_OWNER}
		PRIVATE_SOURCE_RATIONALE
			"${DURIN_ENGINE_TEST_PRIVATE_SOURCE_RATIONALE}"
	)
	durin_discover_tests(${target_name})
endfunction()

durin_add_engine_functional_test(EditorPropertyTests
	KIND feature
	DOMAINS property-editor
	MODULES durin-ed level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned property customization white-box coverage avoids exporting editor implementation symbols."
	RUNTIME_STACK_RATIONALE "Exercises DurinEd property customization behavior."
	SOURCES
		Private/ReflectedPropertyViewTests.cpp
		Private/Editor/ReflectedPropertyEditSessionTests.cpp
		Private/Editor/ReflectedPropertyTransactionTests.cpp
		Private/Editor/ReflectedPropertyContainerTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	LIBRARIES DurinEd
)

durin_add_engine_functional_test(EditorAssetWorkflowTests
	KIND feature
	DOMAINS asset-workflow
	MODULES durin-ed level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned asset workflow white-box coverage keeps transaction and browser implementation seams private."
	RUNTIME_STACK_RATIONALE "Exercises editor asset workflows across DurinEd and Mona UI models."
	SOURCES
		Private/Editor/WorkspaceAssetOpenCompatibilityTests.cpp
		Private/Editor/AssetCompatibilityAuditTests.cpp
		Private/Editor/AssetDestinationValidationTests.cpp
		Private/Editor/ImportDialogStateTests.cpp
		Private/Editor/ContentBrowserItemViewTests.cpp
		Private/Editor/ContentBrowserModelTests.cpp
		Private/Editor/ContentBrowserRefreshCoordinatorTests.cpp
		Private/SourceLibraryReferenceContractTests.cpp
		Private/SourceReferenceIndexTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Assets/AssetRelocationTransaction.cpp
		${_durin_level_editor_private}/Assets/AssetDestinationValidation.cpp
		${_durin_level_editor_private}/Assets/ImportDialogState.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDecoder.cpp
		${_durin_level_editor_private}/Panels/ContentBrowserItemView.cpp
		${_durin_level_editor_private}/Panels/ContentBrowserModel.cpp
		${_durin_level_editor_private}/Panels/ContentBrowserOperations.cpp
		${_durin_level_editor_private}/Panels/ContentBrowserRefreshCoordinator.cpp
		${_durin_level_editor_private}/Panels/ContentDeletionTransaction.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd EngineAssetBuild StandardAssetImport
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(AssetReferenceStoreTests
	KIND contract
	DOMAINS asset-reference
	MODULES level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned project reference-store white-box coverage avoids exporting private settings symbols."
	RUNTIME_STACK_RATIONALE "Exercises production external asset-reference stores."
	SOURCES
		Private/Editor/ProjectDefaultLevelReferenceStoreTests.cpp
		Private/Editor/ProjectGameSettingsTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Settings/ProjectDefaultLevelReferenceStore.cpp
)

durin_add_engine_functional_test(EditorHierarchyTests
	KIND feature
	DOMAINS hierarchy
	MODULES level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned hierarchy model white-box coverage avoids exporting its private model implementation."
	RUNTIME_STACK_RATIONALE "Exercises the deterministic Level Editor hierarchy model without editor startup."
	SOURCES
		Private/Editor/WorldOutlinerHierarchyModelTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Panels/WorldOutlinerHierarchyModel.cpp
)

durin_add_engine_functional_test(LevelAuthoringTests
	KIND feature
	DOMAINS world
	MODULES durin-ed level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned authoring white-box coverage keeps structural transaction implementations private."
	COMPILE_DEFINITIONS DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION=1
	RUNTIME_STACK_RATIONALE "Exercises transaction-backed LevelEditor structural authoring."
	SOURCES
		Private/Editor/StaticMeshLevelAuthoringTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Authoring/StaticMeshLevelAuthoring.cpp
		${_durin_level_editor_private}/Authoring/GrayboxSceneAuthoring.cpp
	LIBRARIES DurinEd
)

durin_add_engine_functional_test(MaterialTests
	KIND integration
	DOMAINS material
	MODULES engine material-editor renderer
	BACKENDS vulkan
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER MaterialEditor
	PRIVATE_SOURCE_RATIONALE
		"MaterialEditor-owned preview and panel white-box coverage avoids widening the editor module API."
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Exercises rendered material editing and preview lifecycle."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for this Vulkan-backed test."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES
		Private/Materials/MaterialSchemaAndEditingTests.cpp
		Private/Materials/MaterialDependencyTests.cpp
		Private/Materials/MaterialRenderProxyTests.cpp
		Private/Materials/MaterialInstanceTests.cpp
		Private/Materials/MaterialRenderingTests.cpp
		Private/Materials/MaterialRenderRepresentationTests.cpp
		Private/MaterialParameterPanelModelTests.cpp
	PRIVATE_SOURCES
		${_durin_material_editor_private}/Widgets/MaterialPreview.cpp
		${_durin_material_editor_private}/Widgets/MaterialParameterPanelModel.cpp
	LIBRARIES
		ApplicationCore
		AssetImportCore
		RenderCore
		Renderer
		StandardAssetImport
		MonaCore
		Mona
		MonaImGui
		DurinEd
		MaterialEditor
		StaticMeshEditor
		TextureEditor
		EngineAssetBuild
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(StaticMeshTests
	KIND feature
	DOMAINS static-mesh
	MODULES engine level-editor static-mesh-editor
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned static-mesh details white-box coverage avoids exporting private customization symbols."
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises renderer-backed static-mesh editing and derived data."
	SOURCES
		Private/Materials/StaticMeshMaterialTests.cpp
		Private/Materials/StaticMeshRenderDataLifetimeContractTests.cpp
		Private/Materials/StaticMeshUpdateTests.cpp
		Private/StaticMeshTestEnvironment.cpp
		Private/StaticMeshDerivedDataContractTests.cpp
		Private/StaticMeshDerivedDataCacheTests.cpp
		Private/StaticMeshPayloadCodecTests.cpp
		Private/StaticMeshCollisionRoutineTests.cpp
		Private/StaticMeshMaterialSlotDetailsTests.cpp
		Private/StaticMeshEditorTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Customizations/StaticMeshMaterialSlotDetails.cpp
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	LIBRARIES AssetImportCore EngineAssetBuild StandardAssetImport RenderCore Renderer DurinEd StaticMeshEditor
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
)

durin_add_engine_functional_test(StaticMeshCollisionQualificationTests
	KIND qualification
	DOMAINS physics static-mesh
	MODULES aether engine static-mesh-editor
	STACKS editor renderer
	RUNTIME_STACK_RATIONALE "Measures the large cooked-collision prototype while preserving its frozen helper model."
	SOURCES Private/StaticMeshCollisionQualificationTests.cpp
	LIBRARIES AssetImportCore EngineAssetBuild StandardAssetImport RenderCore Renderer DurinEd StaticMeshEditor
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
)

durin_add_engine_functional_test(SkeletalAssetTests
	KIND feature
	DOMAINS skeletal-mesh
	MODULES engine
	SOURCES
		Private/SkeletalAssetTests.cpp
		Private/SkeletalAnimationTests.cpp
)

durin_add_engine_functional_test(SkeletalMeshEditorTests
	KIND feature
	DOMAINS skeletal-mesh
	MODULES engine skeletal-mesh-editor
	STACKS editor renderer
	RUNTIME_STACK_RATIONALE "Exercises exact skeletal asset editor registration and read-only ownership."
	SOURCES Private/SkeletalMeshEditorTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd SkeletalMeshEditor
)

durin_add_engine_functional_test(SkeletalSceneLifecycleTests
	KIND integration
	DOMAINS asset-import skeletal-mesh
	MODULES engine skeletal-mesh-editor standard-asset-import
	BACKENDS vulkan
	STACKS editor renderer
	GPU
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises editor skeletal Scene import through cook and runtime-only load."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for cold-DDC skeletal reimport resource validation."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES
		Private/SkeletalSceneLifecycleTests.cpp
	LIBRARIES AssetImportCore StandardAssetImport RenderCore DurinEd SkeletalMeshEditor
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
)

durin_add_engine_functional_test(SkeletalMeshRenderResourcesVulkanTests
	KIND integration
	DOMAINS skeletal-mesh
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Exercises Vulkan-backed skeletal render-resource initialization and retry."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for skeletal render-resource validation."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/SkeletalMeshRenderResourcesVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
)

set(_durin_texture_test_include_directories
	${CMAKE_CURRENT_SOURCE_DIR}/Private
	${DURIN_PROJECT_ROOT_DIR}/Source/Editor/StandardAssetImport/Private
)
set(_durin_texture_test_libraries
	Core
	CoreDObject
	AssetCore
	Engine
	AssetImportCore
	EngineAssetBuild
	StandardAssetImport
	RenderCore
	Renderer
	DurinEd
)

if(DURIN_WITH_EDITOR)
	add_durin_test(TextureTests
		Private/Texture/TextureImportAndCacheTests.cpp
		Private/Texture/TextureDerivedDataTests.cpp
		Private/Texture/TextureBuildTests.cpp
		Private/Texture/Texture2DBuildCoordinatorTests.cpp
		Private/Texture/TextureFailureTests.cpp
		Private/Texture/SingleAssetImportTests.cpp
		Private/Texture/EquirectangularTextureCubeTests.cpp
		Private/TextureCubeTests.cpp
	)
	target_include_directories(TextureTests PRIVATE
		${_durin_texture_test_include_directories})
	target_link_libraries(TextureTests PRIVATE
		${_durin_texture_test_libraries})
	set_target_properties(TextureTests PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
		DURIN_TEST_HEAVY_RUNTIME_RATIONALE
			"Exercises editor texture import, build, cache, and render-resource contracts."
		DURIN_TEST_TIMEOUT 600
	)
	durin_test_deploy_directory_to_data(
		TextureTests
		"${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport"
	)
	durin_test_deploy_directory_to_data(
		TextureTests
		"${CMAKE_CURRENT_SOURCE_DIR}/Data"
	)
	durin_finalize_native_test(TextureTests
		KIND feature
		DOMAINS texture
		MODULES engine engine-asset-build standard-asset-import
		STACKS editor
	)
	durin_discover_tests(TextureTests)

	add_durin_test(SceneImportTests
		Private/Texture/SceneImportTests.cpp
	)
	target_include_directories(SceneImportTests PRIVATE
		${_durin_texture_test_include_directories})
	target_link_libraries(SceneImportTests PRIVATE
		${_durin_texture_test_libraries})
	set_target_properties(SceneImportTests PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
		DURIN_TEST_HEAVY_RUNTIME_RATIONALE
			"Exercises editor scene-import publication and rollback across runtime asset families."
		DURIN_TEST_TIMEOUT 600
	)
	durin_test_deploy_directory_to_data(
		SceneImportTests
		"${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport"
	)
	durin_finalize_native_test(SceneImportTests
		KIND integration
		DOMAINS asset-import
		MODULES asset-import-core engine standard-asset-import
		STACKS editor
	)
	durin_discover_tests(SceneImportTests)
else()
	durin_exclude_native_test_sources(
		RATIONALE "Texture authoring and scene import require editor-only Build and StandardAssetImport services."
		SOURCES
			Private/Texture/TextureImportAndCacheTests.cpp
			Private/Texture/TextureDerivedDataTests.cpp
			Private/Texture/TextureBuildTests.cpp
			Private/Texture/Texture2DBuildCoordinatorTests.cpp
			Private/Texture/TextureFailureTests.cpp
			Private/Texture/SceneImportTests.cpp
			Private/Texture/SingleAssetImportTests.cpp
			Private/Texture/EquirectangularTextureCubeTests.cpp
			Private/TextureCubeTests.cpp
	)
endif()

durin_add_engine_functional_test(TerrainHeightmapTests
	KIND feature
	DOMAINS terrain
	MODULES engine engine-asset-build standard-asset-import
	STACKS editor
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises heightmap import, DDC, package, and source-index integration."
	SOURCES Private/Terrain/TerrainHeightmapTests.cpp
	LIBRARIES AssetImportCore StandardAssetImport DurinEd EngineAssetBuild
)

durin_add_engine_functional_test(TerrainHeightmapCookTests
	KIND integration
	DOMAINS asset-cook terrain
	MODULES engine engine-asset-build standard-asset-import
	STACKS editor
	EDITOR_ONLY
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises source-free cooked heightmap package loading."
	SOURCES Private/Terrain/TerrainHeightmapCookTests.cpp
	LIBRARIES EngineAssetBuild StandardAssetImport
)

durin_add_engine_functional_test(TerrainRenderPrimitiveTests
	KIND contract
	DOMAINS terrain
	MODULES engine renderer
	STACKS renderer
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises reflected Terrain component, patch, proxy, and revision contracts."
	SOURCES Private/Terrain/TerrainRenderPrimitiveTests.cpp
	LIBRARIES Renderer RenderCore
)

durin_add_engine_functional_test(TerrainRenderVulkanTests
	KIND integration
	DOMAINS terrain
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Exercises exact R16 Terrain rendering, counters, and resource release on Vulkan."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for Terrain render validation."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/Terrain/TerrainRenderVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
)

durin_add_engine_functional_test(TerrainRenderQualificationTests
	KIND qualification
	DOMAINS terrain
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE
		"Measures maximum supported Terrain rendering on an initialized Vulkan device."
	RUNTIME_ONLY_RATIONALE
		"RHIInit selects VulkanRHI dynamically for Terrain render qualification."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/Terrain/TerrainRenderQualificationTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
)

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

durin_add_engine_functional_test(EnvironmentLightingTests
	KIND feature
	DOMAINS environment-lighting
	MODULES engine
	RUNTIME_STACK_RATIONALE "Exercises built-in environment payload and direct cook behavior."
	SOURCES Private/EnvironmentLightingTests.cpp
)

durin_add_engine_functional_test(RendererSceneContractTests
	KIND contract
	DOMAINS renderer
	MODULES engine renderer
	STACKS renderer
	RUNTIME_STACK_RATIONALE "Exercises renderer-owned SceneProxy and SceneInfo lifetime contracts."
	SOURCES Private/RendererSceneContractTests.cpp
	LIBRARIES RenderCore Renderer
)

durin_add_engine_functional_test(SceneImportVulkanTests
	KIND integration
	DOMAINS asset-import
	MODULES engine renderer standard-asset-import vulkan-rhi
	BACKENDS vulkan
	STACKS editor renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Owns the Vulkan-backed static-model import acceptance lifecycle."
	SOURCES Private/Texture/SceneImportVulkanTests.cpp
	LIBRARIES ApplicationCore AssetImportCore EngineAssetBuild StandardAssetImport RenderCore Renderer DurinEd TextureEditor VulkanRHI Vulkan::Vulkan
	INCLUDE_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Source/Editor/StandardAssetImport/Private
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
	COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(ThumbnailTests
	KIND feature
	DOMAINS thumbnail
	MODULES engine level-editor
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned thumbnail cache white-box coverage avoids exporting private cache implementations."
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises renderer-backed editor thumbnail generation and caching."
	SOURCES
		Private/SourceImageThumbnailTests.cpp
		Private/AssetThumbnailContractTests.cpp
		Private/RenderedAssetThumbnailFixtureTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Assets/SourceImageThumbnailCache.cpp
		${_durin_level_editor_private}/Assets/ContentBrowserThumbnailCache.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDecoder.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDiskCache.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd EngineAssetBuild StandardAssetImport StaticMeshEditor TextureEditor
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(MaterialThumbnailTests
	KIND feature
	DOMAINS material thumbnail
	MODULES engine material-editor
	STACKS editor renderer
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises the MaterialEditor-owned Material and MaterialInstance thumbnail extensions."
	SOURCES Private/MaterialAssetThumbnailTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd EngineAssetBuild StandardAssetImport MaterialEditor TextureEditor
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(TextureThumbnailTests
	KIND feature
	DOMAINS texture thumbnail
	MODULES engine texture-editor
	STACKS editor renderer
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises the TextureEditor-owned Texture2D and TextureCube thumbnail extensions."
	SOURCES Private/TextureAssetThumbnailTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd EngineAssetBuild StandardAssetImport TextureEditor
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(StaticMeshThumbnailTests
	KIND feature
	DOMAINS static-mesh thumbnail
	MODULES engine level-editor static-mesh-editor
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned thumbnail cache white-box coverage accompanies the StaticMeshEditor extension without exporting private symbols."
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises the StaticMeshEditor-owned thumbnail extension and cache lifecycle."
	SOURCES Private/StaticMeshAssetThumbnailTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Assets/SourceImageThumbnailCache.cpp
		${_durin_level_editor_private}/Assets/ContentBrowserThumbnailCache.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDecoder.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDiskCache.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd EngineAssetBuild StandardAssetImport MaterialEditor StaticMeshEditor TextureEditor
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

set(_durin_world_test_sources
	Private/World/NewLevelBaselineTests.cpp
	Private/World/WorldPlayTests.cpp
	Private/World/WorldLifecycleMutationTests.cpp
	Private/World/WorldTickSchedulingTests.cpp
	Private/World/WorldActorIteratorTests.cpp
	Private/World/WorldActorTests.cpp
	Private/World/WorldComponentTests.cpp
	Private/World/WorldLifetimeTests.cpp
	Private/World/NativeGameplayCoreTests.cpp
)
if(DURIN_WITH_EDITOR)
	add_durin_test(WorldTests ${_durin_world_test_sources})
	target_include_directories(WorldTests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/Private
	)
	target_link_libraries(WorldTests PRIVATE
		Engine
		DurinEd
		StandardAssetImport
	)
	set_target_properties(WorldTests PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
		DURIN_TEST_HEAVY_RUNTIME_RATIONALE
			"Exercises DurinEd world editing integration."
	)
	durin_finalize_native_test(WorldTests
		KIND feature
		DOMAINS world
		MODULES engine durin-ed
		STACKS editor
	)
	durin_discover_tests(WorldTests)
else()
	durin_exclude_native_test_sources(
		RATIONALE "WorldTests requires the DurinEd editor world lifecycle."
		SOURCES ${_durin_world_test_sources}
	)
endif()

durin_add_engine_functional_test(PhysicsSceneTests
	KIND feature
	DOMAINS physics
	MODULES aether engine engine-asset-build
	STACKS editor
	SOURCES
		Private/Physics/PhysicsSceneTests.cpp
		Private/Physics/PhysicsQueryObservabilityTests.cpp
	LIBRARIES AetherCore Aether EngineAssetBuild
)

durin_add_engine_functional_test(PhysicsQualificationTests
	KIND qualification
	DOMAINS physics
	MODULES aether engine
	SOURCES Private/Physics/PhysicsQualificationTests.cpp
	LIBRARIES AetherCore Aether
)

add_durin_test(MonaViewportTests
	Private/Viewport/ViewportDisplaySourceTests.cpp
)
target_link_libraries(MonaViewportTests PRIVATE
	Core
	RHI
	MonaCore
	Mona
)
set_target_properties(MonaViewportTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
	DURIN_TEST_HEAVY_RUNTIME_RATIONALE
		"Exercises the Mona display-source consumer without Engine linkage."
)
durin_finalize_native_test(MonaViewportTests
	KIND contract
	DOMAINS viewport
	MODULES mona
)
durin_discover_tests(MonaViewportTests)

add_durin_test(EngineViewportHeaderTests
	Private/Viewport/EngineViewportHeaderTests.cpp
)
target_link_libraries(EngineViewportHeaderTests PRIVATE
	Core
	Engine
)
set_target_properties(EngineViewportHeaderTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_finalize_native_test(EngineViewportHeaderTests
	KIND contract
	DOMAINS viewport
	MODULES engine
)
durin_discover_tests(EngineViewportHeaderTests)

set(_durin_viewport_test_sources
	Private/Viewport/ViewportFoundationTests.cpp
	Private/Viewport/ViewportProjectionTests.cpp
	Private/Viewport/ViewportCustomizationTests.cpp
	Private/Viewport/ViewportInteractionTests.cpp
	Private/Viewport/ViewportPickingContractTests.cpp
	Private/Viewport/DetailsSelectionTests.cpp
)
set(_durin_viewport_private_sources
	${_durin_level_editor_private}/Panels/DetailsPanelTargeting.cpp
	${_durin_level_editor_private}/Viewport/ViewportCameraTransform.cpp
	${_durin_level_editor_private}/Viewport/CameraPreviewViewportClient.cpp
	${_durin_level_editor_private}/Viewport/LevelEditorViewportClient.cpp
	${_durin_level_editor_private}/Viewport/ViewportPickingService.cpp
	${_durin_level_editor_private}/Viewport/ViewportPickingSceneIndex.cpp
	${_durin_level_editor_private}/Viewport/LevelEditorViewportEditing.cpp
	${_durin_level_editor_private}/Viewport/TransformGizmo.cpp
	${_durin_level_editor_private}/Customizations/CameraEditorCustomizations.cpp
	${_durin_level_editor_private}/Customizations/DirectionalLightEditorCustomizations.cpp
	${_durin_level_editor_private}/Customizations/PlayerStartEditorCustomizations.cpp
	${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	${_durin_level_editor_private}/Customizations/SplineEditorCustomizations.cpp
	${_durin_level_editor_private}/Settings/LevelViewportSessionSettings.cpp
	${_durin_level_editor_private}/Workspace/LevelEditorContext.cpp
)
if(DURIN_WITH_EDITOR)
	add_durin_test(ViewportTests
		${_durin_viewport_test_sources}
		${_durin_viewport_private_sources}
	)
	target_include_directories(ViewportTests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/MonaImGui/Private
	)
	target_compile_definitions(ViewportTests PRIVATE LEVELEDITOR_EXPORTS)
	target_link_libraries(ViewportTests PRIVATE
		Core
		CoreDObject
		RHI
		RenderCore
		Engine
		ApplicationCore
		MonaCore
		Mona
		MonaImGui
		AssetCore
		AssetImportCore
		StandardAssetImport
		EngineAssetBuild
		DurinEd
	)
	set_target_properties(ViewportTests PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
		DURIN_TEST_HEAVY_RUNTIME_RATIONALE
			"Exercises DurinEd and Mona viewport interaction behavior."
		DURIN_TEST_TIMEOUT 600
	)
	durin_finalize_native_test(ViewportTests
		KIND feature
		DOMAINS viewport
		MODULES engine level-editor mona
		STACKS editor
		PRIVATE_SOURCE_OWNER LevelEditor
		PRIVATE_SOURCE_RATIONALE
			"LevelEditor-owned viewport white-box coverage without exporting private DLL symbols."
	)
	durin_discover_tests(ViewportTests)

	add_durin_test(ViewportQualificationTests
		Private/Viewport/ViewportPickingQualificationTests.cpp
		${_durin_viewport_private_sources}
	)
	target_include_directories(ViewportQualificationTests PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/MonaImGui/Private
	)
	target_compile_definitions(ViewportQualificationTests PRIVATE LEVELEDITOR_EXPORTS)
	target_link_libraries(ViewportQualificationTests PRIVATE
		Core
		CoreDObject
		RHI
		RenderCore
		Engine
		ApplicationCore
		MonaCore
		Mona
		MonaImGui
		AssetCore
		AssetImportCore
		StandardAssetImport
		EngineAssetBuild
		DurinEd
	)
	set_target_properties(ViewportQualificationTests PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
		DURIN_TEST_HEAVY_RUNTIME_RATIONALE
			"Measures large-scale editor viewport picking behavior."
		DURIN_TEST_TIMEOUT 900
	)
	durin_finalize_native_test(ViewportQualificationTests
		KIND qualification
		DOMAINS viewport
		MODULES engine level-editor mona
		STACKS editor
		PRIVATE_SOURCE_OWNER LevelEditor
		PRIVATE_SOURCE_RATIONALE
			"LevelEditor-owned viewport qualification uses the same private picking seams as routine coverage."
	)
	durin_discover_tests(ViewportQualificationTests)
else()
	durin_exclude_native_test_sources(
		RATIONALE "ViewportTests requires LevelEditor composition and DurinEd."
		SOURCES
			${_durin_viewport_test_sources}
			Private/Viewport/ViewportPickingQualificationTests.cpp
	)
endif()

durin_add_engine_functional_test(SplineTests
	KIND feature
	DOMAINS spline
	MODULES engine level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned spline editing white-box coverage avoids exporting private viewport and customization symbols."
	RUNTIME_STACK_RATIONALE "Exercises DurinEd spline customization behavior."
	SOURCES
		Private/SplineTests.cpp
		Private/SplineMeshComponentTests.cpp
		Private/SplineMeshDeformerTests.cpp
		Private/SplineV2ContractTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Customizations/SplineEditorCustomizations.cpp
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
		${_durin_level_editor_private}/Viewport/ViewportCameraTransform.cpp
		${_durin_level_editor_private}/Viewport/LevelEditorViewportClient.cpp
		${_durin_level_editor_private}/Viewport/ViewportPickingService.cpp
		${_durin_level_editor_private}/Viewport/ViewportPickingSceneIndex.cpp
		${_durin_level_editor_private}/Viewport/LevelEditorViewportEditing.cpp
		${_durin_level_editor_private}/Viewport/TransformGizmo.cpp
		${_durin_level_editor_private}/Workspace/LevelEditorContext.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd AssetImportCore StandardAssetImport
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
)

durin_add_engine_functional_test(SplineQualificationTests
	KIND qualification
	DOMAINS spline
	MODULES engine
	SOURCES Private/SplineQualificationTests.cpp
)

durin_add_engine_functional_test(SkyBoxTests
	KIND feature
	DOMAINS sky-box
	MODULES engine level-editor renderer
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned sky-box authoring white-box coverage avoids exporting private customization symbols."
	RUNTIME_STACK_RATIONALE "Exercises renderer-backed sky-box editing and rendering contracts."
	SOURCES
		Private/SkyBox/SkyBoxRenderingTests.cpp
		Private/SkyBox/SkyBoxComponentTests.cpp
		Private/SkyBox/SkyBoxEditorTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Authoring/SkyBoxLevelAuthoring.cpp
		${_durin_level_editor_private}/Customizations/SkyBoxDetails.cpp
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	LIBRARIES RenderCore Renderer DurinEd
	DATA_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(SkyBoxVulkanIntegrationTests
	KIND integration
	DOMAINS sky-box
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Owns the Vulkan-backed sky-box integration lifecycle."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for this Vulkan-backed test."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/SkyBox/SkyBoxVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd
	DATA_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(RendererResourceReloadVulkanTests
	KIND integration
	DOMAINS renderer shader
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Exercises in-process renderer shader failure, reload, and recovery on Vulkan."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for this Vulkan-backed test."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/RendererResourceReloadVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
)

durin_add_engine_functional_test(StaticMeshRenderPreparationVulkanTests
	KIND integration
	DOMAINS static-mesh
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Exercises view-local StaticMesh material preparation against initialized render resources."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for the prepared-section resource gate."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/StaticMeshRenderPreparationVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
)

durin_add_engine_functional_test(EditorRenderingTests
	KIND feature
	DOMAINS renderer
	MODULES durin-ed engine renderer
	STACKS editor renderer
	RUNTIME_STACK_RATIONALE "Exercises renderer-backed editor assistance and grid rendering."
	SOURCES
		Private/EditorGridRenderingTests.cpp
		Private/RendererEditorAssistanceTests.cpp
		Private/RendererResourceInvalidationTests.cpp
		Private/RendererResourceSlotCacheTests.cpp
		Private/RendererRenderTargetLayoutTests.cpp
		Private/RendererSceneViewTests.cpp
		Private/EditorTextureSmokeTests.cpp
	LIBRARIES ApplicationCore AssetImportCore StandardAssetImport RenderCore Renderer DurinEd MaterialEditor EngineAssetBuild
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(EditorShellTests
	KIND feature
	DOMAINS editor-shell
	MODULES durin-ed level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned shell model white-box coverage avoids exporting private workspace and panel implementations."
	RUNTIME_STACK_RATIONALE "Exercises DurinEd and Mona editor-shell models."
	SOURCES
		Private/EditorBootstrapStateTests.cpp
		Private/EditorNotificationTests.cpp
		Private/EditorWorkspaceTests.cpp
		Private/UIStyleTests.cpp
		Private/ConsoleRecordModelTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Workspace/LevelEditorContext.cpp
		${_durin_level_editor_private}/Viewport/ViewportPickingSceneIndex.cpp
		${_durin_level_editor_private}/Panels/ConsoleRecordModel.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd
)

durin_add_engine_functional_test(ExternalToolTests
	KIND feature
	DOMAINS editor-shell
	MODULES durin-ed main-frame
	STACKS editor
	PRIVATE_SOURCE_OWNER MainFrame
	PRIVATE_SOURCE_RATIONALE
		"MainFrame-owned profiling integration white-box coverage avoids exporting the private service implementation."
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises the DurinEd profiling-tool integration."
	SOURCES Private/ProfilingToolServiceTests.cpp
	PRIVATE_SOURCES
		${_durin_main_frame_private}/ProfilingToolService.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd
)

# Cooked-runtime mode and Renderer/Vulkan teardown are process-global.
durin_add_engine_functional_test(TextureCookIntegrationTests
	KIND integration
	DOMAINS asset-cook texture
	MODULES engine engine-asset-build renderer standard-asset-import
	BACKENDS vulkan
	STACKS editor renderer
	EDITOR_ONLY
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Owns the renderer and Vulkan cooked-texture lifecycle."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for this Vulkan-backed test."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/Texture/TextureCookTests.cpp
	LIBRARIES EngineAssetBuild StandardAssetImport RenderCore Renderer
)
