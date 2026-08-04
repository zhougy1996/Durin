include(GoogleTest)

set(_durin_level_editor_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private")
set(_durin_material_editor_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Private")
set(_durin_main_frame_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MainFrame/Private")

function(durin_add_engine_functional_test target_name)
	set(options GPU)
	set(one_value_args TIMEOUT RUNTIME_STACK_RATIONALE)
	set(multi_value_args
		SOURCES
		PRIVATE_SOURCES
		LIBRARIES
		DEPLOY_TARGETS
		RUNTIME_FILES
		DATA_DIRECTORIES
	)
	cmake_parse_arguments(
		DURIN_ENGINE_TEST
		"${options}"
		"${one_value_args}"
		"${multi_value_args}"
		${ARGN}
	)

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
		${_durin_main_frame_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source
	)
	target_compile_definitions(${target_name} PRIVATE LEVELEDITOR_EXPORTS)
	target_link_libraries(${target_name} PRIVATE
		Core
		CoreDObject
		AssetCore
		Engine
		bc7enc_rdo::bc7enc_rdo
		${DURIN_ENGINE_TEST_LIBRARIES}
	)

	foreach(_durin_deploy_target IN ITEMS
		Core
		CoreDObject
		AssetCore
		Engine
		${DURIN_ENGINE_TEST_DEPLOY_TARGETS}
	)
		durin_test_deploy_target_binary(
			${target_name}
			${_durin_deploy_target}
		)
	endforeach()
	foreach(_durin_runtime_file IN LISTS DURIN_ENGINE_TEST_RUNTIME_FILES)
		durin_test_deploy_runtime_files(
			${target_name}
			${_durin_runtime_file}
		)
	endforeach()
	foreach(_durin_data_directory IN LISTS DURIN_ENGINE_TEST_DATA_DIRECTORIES)
		durin_test_deploy_directory_to_data(
			${target_name}
			"${_durin_data_directory}"
		)
	endforeach()

	if(DURIN_ENGINE_TEST_GPU)
		set_target_properties(${target_name} PROPERTIES
			DURIN_TEST_LEGACY_SERIALIZATION_GROUP renderer-runtime
			DURIN_TEST_RESOURCE_LOCKS durin-gpu
		)
	endif()
	if(DURIN_ENGINE_TEST_TIMEOUT)
		set_target_properties(${target_name} PROPERTIES
			DURIN_TEST_TIMEOUT "${DURIN_ENGINE_TEST_TIMEOUT}"
		)
	endif()

	durin_discover_tests(${target_name})
endfunction()

durin_add_engine_functional_test(EditorPropertyTests
	RUNTIME_STACK_RATIONALE "Exercises DurinEd property customization behavior."
	SOURCES
		Private/ReflectedPropertyViewTests.cpp
		Private/Editor/ReflectedPropertyEditSessionTests.cpp
		Private/Editor/ReflectedPropertyTransactionTests.cpp
		Private/Editor/ReflectedPropertyContainerTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	LIBRARIES DurinEd
	DEPLOY_TARGETS DurinEd
)

durin_add_engine_functional_test(EditorAssetWorkflowTests
	RUNTIME_STACK_RATIONALE "Exercises editor asset workflows across DurinEd and Mona UI models."
	SOURCES
		Private/Editor/AssetStructureUpgradeModelTests.cpp
		Private/Editor/AssetUpgradeAuditServiceTests.cpp
		Private/Editor/AssetDestinationValidationTests.cpp
		Private/Editor/ImportDialogStateTests.cpp
		Private/Editor/ContentBrowserItemViewTests.cpp
		Private/Editor/ContentBrowserModelTests.cpp
		Private/SourceLibraryReferenceContractTests.cpp
		Private/SourceReferenceIndexTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Documents/AssetStructureUpgradeModel.cpp
		${_durin_level_editor_private}/Assets/AssetDestinationValidation.cpp
		${_durin_level_editor_private}/Assets/ImportDialogState.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDecoder.cpp
		${_durin_level_editor_private}/Panels/ContentBrowserItemView.cpp
		${_durin_level_editor_private}/Panels/ContentBrowserModel.cpp
		${_durin_level_editor_private}/Panels/ContentBrowserOperations.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd
	DEPLOY_TARGETS ApplicationCore RHI RenderCore Renderer MonaCore Mona MonaImGui DurinEd
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(EditorHierarchyTests
	RUNTIME_STACK_RATIONALE "Exercises the deterministic Level Editor hierarchy model without editor startup."
	SOURCES
		Private/Editor/WorldOutlinerHierarchyModelTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Panels/WorldOutlinerHierarchyModel.cpp
)

durin_add_engine_functional_test(MaterialTests
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Exercises rendered material editing and preview lifecycle."
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
		bc7enc_rdo::bc7enc_rdo
	DEPLOY_TARGETS
		ApplicationCore
		AssetImportCore
		RHI
		RenderCore
		Renderer
		StandardAssetImport
		MonaCore
		Mona
		MonaImGui
		VulkanRHI
		DurinEd
	RUNTIME_FILES assimp::assimp Slang_Imported
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(StaticMeshTests
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
		Private/StaticMeshMaterialSlotDetailsTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Customizations/StaticMeshMaterialSlotDetails.cpp
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	LIBRARIES AssetImportCore StandardAssetImport RenderCore Renderer DurinEd bc7enc_rdo::bc7enc_rdo
	DEPLOY_TARGETS AssetImportCore StandardAssetImport RHI RenderCore Renderer DurinEd
	RUNTIME_FILES assimp::assimp
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
)

durin_add_engine_functional_test(TextureTests
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises renderer-backed texture import, build, and cache behavior."
	SOURCES
		Private/Texture/TextureImportAndCacheTests.cpp
		Private/Texture/TextureDerivedDataTests.cpp
		Private/Texture/TextureBuildTests.cpp
		Private/Texture/TextureFailureTests.cpp
		Private/Texture/SceneImportTests.cpp
		Private/Texture/SingleAssetImportTests.cpp
		Private/Texture/EquirectangularTextureCubeTests.cpp
		Private/TextureCubeTests.cpp
	LIBRARIES AssetImportCore StandardAssetImport RenderCore Renderer DurinEd bc7enc_rdo::bc7enc_rdo
	DEPLOY_TARGETS AssetImportCore StandardAssetImport RHI RenderCore Renderer DurinEd
	RUNTIME_FILES assimp::assimp Slang_Imported
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
	)
target_include_directories(TextureTests PRIVATE
	${DURIN_PROJECT_ROOT_DIR}/Source/Editor/StandardAssetImport/Private)

durin_add_engine_functional_test(SceneImportVulkanTests
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Owns the Vulkan-backed static-model import acceptance lifecycle."
	SOURCES Private/Texture/SceneImportVulkanTests.cpp
	LIBRARIES ApplicationCore AssetImportCore StandardAssetImport RenderCore Renderer DurinEd bc7enc_rdo::bc7enc_rdo
	DEPLOY_TARGETS ApplicationCore AssetImportCore StandardAssetImport RHI RenderCore Renderer VulkanRHI DurinEd
	RUNTIME_FILES assimp::assimp Slang_Imported
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)
target_include_directories(SceneImportVulkanTests PRIVATE
	${DURIN_PROJECT_ROOT_DIR}/Source/Editor/StandardAssetImport/Private)

durin_add_engine_functional_test(ThumbnailTests
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises renderer-backed editor thumbnail generation and caching."
	SOURCES
		Private/SourceImageThumbnailTests.cpp
		Private/AssetThumbnailContractTests.cpp
		Private/MaterialAssetThumbnailTests.cpp
		Private/TextureCubeAssetThumbnailTests.cpp
		Private/RenderedAssetThumbnailFixtureTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Assets/SourceImageThumbnailCache.cpp
		${_durin_level_editor_private}/Assets/ContentBrowserThumbnailCache.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDecoder.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDiskCache.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd
	DEPLOY_TARGETS ApplicationCore RHI RenderCore Renderer DurinEd
	RUNTIME_FILES Slang_Imported
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(WorldTests
	RUNTIME_STACK_RATIONALE "Exercises DurinEd world editing integration."
	SOURCES
		Private/World/WorldPlayTests.cpp
		Private/World/WorldLifecycleMutationTests.cpp
		Private/World/WorldActorIteratorTests.cpp
		Private/World/WorldActorTests.cpp
		Private/World/WorldComponentTests.cpp
		Private/World/WorldLifetimeTests.cpp
	LIBRARIES DurinEd
	DEPLOY_TARGETS RenderCore DurinEd
)

durin_add_engine_functional_test(ViewportTests
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises DurinEd and Mona viewport interaction behavior."
	SOURCES
		Private/Viewport/ViewportFoundationTests.cpp
		Private/Viewport/ViewportProjectionTests.cpp
		Private/Viewport/ViewportCustomizationTests.cpp
		Private/Viewport/ViewportInteractionTests.cpp
		Private/Viewport/DetailsSelectionTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Panels/DetailsPanelTargeting.cpp
		${_durin_level_editor_private}/Viewport/ViewportCameraTransform.cpp
		${_durin_level_editor_private}/Viewport/CameraPreviewViewportClient.cpp
		${_durin_level_editor_private}/Viewport/LevelEditorViewportClient.cpp
		${_durin_level_editor_private}/Viewport/TransformGizmo.cpp
		${_durin_level_editor_private}/Customizations/CameraEditorCustomizations.cpp
		${_durin_level_editor_private}/Customizations/DirectionalLightEditorCustomizations.cpp
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
		${_durin_level_editor_private}/Customizations/SplineEditorCustomizations.cpp
		${_durin_level_editor_private}/Settings/LevelViewportSessionSettings.cpp
		${_durin_level_editor_private}/Workspace/LevelEditorContext.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd
	DEPLOY_TARGETS ApplicationCore MonaCore Mona MonaImGui DurinEd
)

durin_add_engine_functional_test(SplineTests
	RUNTIME_STACK_RATIONALE "Exercises DurinEd spline customization behavior."
	SOURCES Private/SplineTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Customizations/SplineEditorCustomizations.cpp
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	LIBRARIES DurinEd
	DEPLOY_TARGETS DurinEd
)

durin_add_engine_functional_test(SkyBoxTests
	RUNTIME_STACK_RATIONALE "Exercises renderer-backed sky-box editing and rendering contracts."
	SOURCES
		Private/SkyBox/SkyBoxRenderingTests.cpp
		Private/SkyBox/SkyBoxComponentTests.cpp
		Private/SkyBox/SkyBoxEditorTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Customizations/SkyBoxDetails.cpp
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	LIBRARIES RenderCore Renderer DurinEd
	DEPLOY_TARGETS RHI RenderCore Renderer DurinEd
	RUNTIME_FILES Slang_Imported
	DATA_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(SkyBoxVulkanIntegrationTests
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Owns the Vulkan-backed sky-box integration lifecycle."
	SOURCES Private/SkyBox/SkyBoxVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd
	DEPLOY_TARGETS ApplicationCore RHI RenderCore Renderer VulkanRHI DurinEd
	RUNTIME_FILES Slang_Imported
	DATA_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(RendererResourceReloadVulkanTests
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Exercises in-process renderer shader failure, reload, and recovery on Vulkan."
	SOURCES Private/RendererResourceReloadVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
	DEPLOY_TARGETS ApplicationCore RHI RenderCore Renderer VulkanRHI
	RUNTIME_FILES Slang_Imported
)

durin_add_engine_functional_test(EditorRenderingTests
	RUNTIME_STACK_RATIONALE "Exercises renderer-backed editor assistance and grid rendering."
	SOURCES
		Private/EditorGridRenderingTests.cpp
		Private/RendererEditorAssistanceTests.cpp
		Private/RendererResourceInvalidationTests.cpp
		Private/RendererResourceSlotCacheTests.cpp
		Private/RendererRenderTargetLayoutTests.cpp
		Private/RendererSceneViewTests.cpp
		Private/EditorTextureSmokeTests.cpp
	LIBRARIES ApplicationCore AssetImportCore StandardAssetImport RenderCore Renderer DurinEd
	DEPLOY_TARGETS ApplicationCore AssetImportCore StandardAssetImport RHI RenderCore Renderer DurinEd
	RUNTIME_FILES Slang_Imported
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(EditorShellTests
	RUNTIME_STACK_RATIONALE "Exercises DurinEd and Mona editor-shell models."
	SOURCES
		Private/EditorNotificationTests.cpp
		Private/EditorWorkspaceTests.cpp
		Private/UIStyleTests.cpp
		Private/ConsoleRecordModelTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Workspace/LevelEditorContext.cpp
		${_durin_level_editor_private}/Panels/ConsoleRecordModel.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd
	DEPLOY_TARGETS ApplicationCore MonaCore Mona MonaImGui DurinEd
)

durin_add_engine_functional_test(ExternalToolTests
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises the DurinEd profiling-tool integration."
	SOURCES Private/ProfilingToolServiceTests.cpp
	PRIVATE_SOURCES
		${_durin_main_frame_private}/ProfilingToolService.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd
	DEPLOY_TARGETS ApplicationCore MonaCore Mona MonaImGui DurinEd
)

# Cooked-runtime mode and Renderer/Vulkan teardown are process-global.
durin_add_engine_functional_test(TextureCookIntegrationTests
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Owns the renderer and Vulkan cooked-texture lifecycle."
	SOURCES Private/Texture/TextureCookTests.cpp
	LIBRARIES RenderCore Renderer bc7enc_rdo::bc7enc_rdo
	DEPLOY_TARGETS ApplicationCore RHI RenderCore Renderer VulkanRHI
	RUNTIME_FILES Slang_Imported
)
