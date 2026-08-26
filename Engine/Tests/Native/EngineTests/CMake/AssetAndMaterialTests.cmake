durin_add_engine_functional_test(AssetSaveReadinessTests
	KIND contract
	DOMAINS asset-workflow
	MODULES engine
	SOURCES Private/AssetSaveReadinessTests.cpp
)

durin_add_engine_functional_test(AssetCompilingManagerTests
	KIND contract
	DOMAINS asset-workflow
	MODULES engine
	SOURCES Private/AssetCompilingManagerTests.cpp
)

durin_add_engine_functional_test(DerivedDataCacheTests
	EDITOR_ONLY
	KIND contract
	DOMAINS derived-data
	MODULES derived-data-cache
	RUNTIME_STACK_RATIONALE
		"Exercises the Developer-only derived-data cache contract."
	SOURCES
		Private/DerivedDataCacheTests.cpp
		Private/DerivedDataBuildTests.cpp
	LIBRARIES DerivedDataCache
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

durin_add_engine_functional_test(EditorOperationTests
	KIND contract
	DOMAINS editor-operation
	MODULES durin-ed
	STACKS editor
	RUNTIME_STACK_RATIONALE
		"Exercises reusable DurinEd asynchronous operation orchestration without an application host."
	SOURCES
		Private/Editor/CompensatingAsyncOperationTests.cpp
	LIBRARIES DurinEd
)

durin_add_engine_functional_test(EditorAssetWorkflowTests
	KIND feature
	DOMAINS asset-workflow
	MODULES durin-ed texture-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER TextureEditor
	PRIVATE_SOURCE_RATIONALE
		"TextureEditor-owned import-form state remains private while its reset and inactive-form behavior is white-box tested."
	RUNTIME_STACK_RATIONALE "Exercises editor asset workflows across DurinEd and Mona UI models."
	SOURCES
		Private/Editor/AssetCompatibilityAuditTests.cpp
		Private/Editor/AssetDestinationValidationTests.cpp
		Private/Editor/ImportDialogStateTests.cpp
		Private/SourceLibraryReferenceContractTests.cpp
		Private/SourceReferenceIndexTests.cpp
	PRIVATE_SOURCES
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/TextureEditor/Private/Import/TextureImportDialogState.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/TextureEditor/Private
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd StaticMeshBuild AssetForgeBuiltins bc7enc_rdo::bc7enc_rdo
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(ContentBrowserWorkflowTests
	KIND feature
	DOMAINS asset-workflow
	MODULES content-browser
	STACKS editor
	PRIVATE_SOURCE_OWNER ContentBrowser
	PRIVATE_SOURCE_RATIONALE
		"ContentBrowser-owned workflow white-box coverage avoids exporting model and operation implementations."
	RUNTIME_STACK_RATIONALE
		"Exercises ContentBrowser asset workflows across AssetCore, DurinEd, and Mona UI models."
	SOURCES
		Private/Editor/ContentBrowserExtensionRegistryTests.cpp
		Private/Editor/ContentBrowserItemViewTests.cpp
		Private/Editor/ContentBrowserModelTests.cpp
		Private/Editor/ContentBrowserRefreshCoordinatorTests.cpp
	PRIVATE_SOURCES
		${_durin_content_browser_private}/ContentBrowserExtensionRegistry.cpp
		${_durin_content_browser_private}/Assets/SourceImageThumbnailDecoder.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserItemView.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserModel.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserOperations.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserRefreshCoordinator.cpp
		${_durin_content_browser_private}/Panels/ContentDeletionTransaction.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd StaticMeshBuild AssetForgeBuiltins bc7enc_rdo::bc7enc_rdo
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
		Private/Materials/MaterialGraphAuthoringTests.cpp
		Private/Materials/MaterialCompileLifecycleTests.cpp
		Private/Materials/MaterialDependencyTests.cpp
		Private/Materials/MaterialRenderProxyTests.cpp
		Private/Materials/MaterialInstanceTests.cpp
		Private/Materials/MaterialRenderingTests.cpp
		Private/Materials/MaterialRenderRepresentationTests.cpp
		Private/MaterialParameterPanelModelTests.cpp
	PRIVATE_SOURCES
		${_durin_material_editor_private}/Graph/MaterialGraphCanvas.cpp
		${_durin_material_editor_private}/Widgets/MaterialPreview.cpp
		${_durin_material_editor_private}/Widgets/MaterialParameterPanelModel.cpp
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/AssetCore/Private
	LIBRARIES
		ApplicationCore
		AssetForge
		RenderCore
		Renderer
		AssetForgeBuiltins
		MonaCore
		Mona
		MonaImGui
		DurinEd
		MaterialEditor
		StaticMeshEditor
		TextureEditor
		StaticMeshBuild
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
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/AssetCore/Private
	LIBRARIES
		ApplicationCore
		AssetForge
		RenderCore
		Renderer
		AssetForgeBuiltins
		MonaCore
		Mona
		MonaImGui
		DurinEd
		MaterialEditor
		StaticMeshEditor
		TextureEditor
		StaticMeshBuild
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(StaticMeshTests
	KIND feature
	DOMAINS static-mesh
	MODULES engine static-mesh-build level-editor static-mesh-editor
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
	LIBRARIES AssetForge StaticMeshBuild AssetForgeBuiltins RenderCore Renderer DurinEd StaticMeshEditor
	INCLUDE_DIRECTORIES
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/AssetCore/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
)

durin_add_engine_functional_test(SkeletalAssetTests
	KIND feature
	DOMAINS skeletal-mesh
	MODULES engine skeletal-build
	RUNTIME_STACK_RATIONALE
		"Exercises uncooked skeletal DDC publication through the editor-only SkeletalBuild module."
	SOURCES
		Private/SkeletalAssetTests.cpp
		Private/SkeletalAnimationTests.cpp
	LIBRARIES SkeletalBuild
)

durin_add_engine_functional_test(SkeletalMeshEditorTests
	KIND feature
	DOMAINS skeletal-mesh
	MODULES engine skeletal-mesh-editor
	STACKS editor renderer
	RUNTIME_STACK_RATIONALE "Exercises exact skeletal asset editor registration and read-only ownership."
	SOURCES Private/SkeletalMeshEditorTests.cpp
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/AssetCore/Private
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd SkeletalMeshEditor
)

durin_add_engine_functional_test(SkeletalSceneLifecycleTests
	KIND integration
	DOMAINS asset-import skeletal-mesh
	MODULES engine skeletal-mesh-editor asset-forge asset-forge-builtins
	STACKS editor renderer
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE
		"Exercises editor skeletal Scene import, render-command publication, cook, and runtime-only load."
	SOURCES
		Private/SkeletalSceneLifecycleTests.cpp
	LIBRARIES AssetForge AssetForgeBuiltins RenderCore DurinEd SkeletalMeshEditor
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
	${DURIN_PROJECT_ROOT_DIR}/Source
	${DURIN_PROJECT_ROOT_DIR}/Source/Developer/TextureBuild/Private
	${DURIN_PROJECT_ROOT_DIR}/Source/Editor/AssetForgeBuiltins/Private
)
set(_durin_texture_test_libraries
	Core
	CoreDObject
	AssetCore
	Engine
	AssetForge
	StaticMeshBuild
	SkeletalBuild
	TerrainBuild
	TextureBuild
	AssetForgeBuiltins
	TextureEditor
	RenderCore
	Renderer
	DurinEd
)
