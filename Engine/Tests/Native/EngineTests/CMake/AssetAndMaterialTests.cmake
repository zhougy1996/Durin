durin_add_engine_functional_test(AssetBuildCoreTests
	EDITOR_ONLY
	KIND contract
	DOMAINS derived-data
	MODULES asset-build-core
	RUNTIME_STACK_RATIONALE
		"Exercises the Developer-only derived-data cache and build-host contracts."
	SOURCES
		Private/AssetBuildCoreTests.cpp
	LIBRARIES AssetBuildCore
)

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
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd GeometryBuild StandardAssetImport bc7enc_rdo::bc7enc_rdo
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
		Private/Editor/WorldOutlinerActorAttachmentTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Authoring/StaticMeshLevelAuthoring.cpp
		${_durin_level_editor_private}/Authoring/TerrainLevelAuthoring.cpp
		${_durin_level_editor_private}/Authoring/GrayboxSceneAuthoring.cpp
		${_durin_level_editor_private}/Panels/ActorAttachmentTransaction.cpp
	LIBRARIES DurinEd
)

durin_add_engine_functional_test(MaterialTests
	KIND feature
	DOMAINS material
	MODULES engine material-editor renderer
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER MaterialEditor
	PRIVATE_SOURCE_RATIONALE
		"MaterialEditor-owned preview and panel white-box coverage avoids widening the editor module API."
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
		MaterialEditor
		StaticMeshEditor
		TextureEditor
		GeometryBuild
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(MaterialVulkanTests
	KIND integration
	DOMAINS material thumbnail
	MODULES engine material-editor renderer static-mesh-editor texture-editor
	BACKENDS vulkan
	STACKS editor renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE
		"Exercises rendered material and thumbnail behavior on Vulkan."
	RUNTIME_ONLY_RATIONALE
		"RHIInit selects VulkanRHI dynamically for the rendered material fixture."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/Materials/MaterialVulkanTests.cpp
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
		GeometryBuild
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
	LIBRARIES AssetImportCore GeometryBuild StandardAssetImport RenderCore Renderer DurinEd StaticMeshEditor
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
)

durin_add_engine_functional_test(SkeletalAssetTests
	KIND feature
	DOMAINS skeletal-mesh
	MODULES engine
	RUNTIME_STACK_RATIONALE
		"Exercises uncooked skeletal DDC publication through the editor-only GeometryBuild module."
	SOURCES
		Private/SkeletalAssetTests.cpp
		Private/SkeletalAnimationTests.cpp
	LIBRARIES GeometryBuild
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
	STACKS editor
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises editor skeletal Scene import through cook and runtime-only load."
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
	GeometryBuild
	TextureBuild
	StandardAssetImport
	RenderCore
	Renderer
	DurinEd
)
