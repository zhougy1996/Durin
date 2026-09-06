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

durin_add_engine_functional_test(CookedMeshLoadingTests
	KIND contract
	DOMAINS asset-workflow static-mesh
	MODULES engine
	SOURCES Private/CookedMeshLoadManagerTests.cpp
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
	MODULES asset-tools durin-ed
	STACKS editor
	RUNTIME_STACK_RATIONALE
		"Exercises reusable DurinEd asynchronous operation orchestration without an application host."
	SOURCES
		Private/Editor/CompensatingAsyncOperationTests.cpp
		Private/Editor/FactoryTests.cpp
		Private/Editor/TransactionRecordTests.cpp
	LIBRARIES AssetTools DurinEd
)

durin_add_engine_functional_test(EditorAssetWorkflowTests
	KIND feature
	DOMAINS asset-workflow
	MODULES asset-maintenance asset-tools content-browser durin-ed main-frame texture-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER TextureEditor
	PRIVATE_SOURCE_RATIONALE
		"TextureEditor-owned import forms and browser detail cache remain private while their state and metadata inspection are white-box tested."
	RUNTIME_STACK_RATIONALE "Exercises editor asset workflows across DurinEd and Mona UI models."
	SOURCES
		Private/Editor/AssetMaintenanceContractTests.cpp
		Private/Editor/AssetCompatibilityAuditTests.cpp
		Private/Editor/AssetDestinationValidationTests.cpp
		Private/Editor/ImportDialogStateTests.cpp
		Private/Editor/TextureCubeDetailsTests.cpp
		Private/SourceLibraryReferenceContractTests.cpp
		Private/SourceReferenceIndexTests.cpp
	PRIVATE_SOURCES
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/TextureEditor/Private/Import/TextureImportDialogState.cpp
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/TextureEditor/Private/ContentBrowser/TextureCubeDetails.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/TextureEditor/Private
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui AssetMaintenance AssetTools ContentBrowser DurinEd MainFrame StaticMeshBuild AssetForgeBuiltins bc7enc_rdo::bc7enc_rdo
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
		"Exercises ContentBrowser asset workflows across Engine Asset, DurinEd, and Mona UI models."
	SOURCES
		Private/Editor/ContentBrowserExtensionRegistryTests.cpp
		Private/Editor/ContentBrowserPanelExtensionTests.cpp
		Private/Editor/ContentBrowserItemViewTests.cpp
		Private/Editor/ContentBrowserModelTests.cpp
		Private/Editor/ContentBrowserRefreshCoordinatorTests.cpp
	PRIVATE_SOURCES
		${_durin_content_browser_private}/ContentBrowserExtensionRegistry.cpp
		${_durin_content_browser_private}/Assets/SourceImageThumbnailDecoder.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserItemView.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserExtensionPresentation.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserPanel.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserPanelView.cpp
		${_durin_content_browser_private}/Assets/ContentBrowserThumbnailReferences.cpp
		${_durin_content_browser_private}/Assets/SourceImageThumbnailCache.cpp
		${_durin_content_browser_private}/Assets/SourceImageThumbnailDiskCache.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserModel.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserQuery.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserDataSource.cpp
		${_durin_content_browser_private}/Operations/ContentBrowserOperationService.cpp
		${_durin_content_browser_private}/Panels/ContentBrowserRefreshCoordinator.cpp
		${_durin_content_browser_private}/Operations/ContentDeletionOperation.cpp
	LIBRARIES ApplicationCore MonaCore Mona MonaImGui DurinEd AssetTools StaticMeshBuild TextureBuild AssetForgeBuiltins bc7enc_rdo::bc7enc_rdo
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

durin_add_engine_functional_test(LevelMutationTests
	KIND feature
	DOMAINS world
	MODULES durin-ed level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned mutation white-box coverage keeps structural transaction implementations private."
	COMPILE_DEFINITIONS DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION=1
	RUNTIME_STACK_RATIONALE "Exercises transaction-backed LevelEditor structural mutation."
	SOURCES
		Private/Editor/StaticMeshLevelMutationTests.cpp
		Private/Editor/WorldOutlinerActorAttachmentTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Operations/StaticMeshLevelMutations.cpp
		${_durin_level_editor_private}/Panels/ActorAttachmentTransaction.cpp
	LIBRARIES DurinEd
)

durin_add_engine_functional_test(MaterialTests
	KIND feature
	DOMAINS material
	MODULES asset-tools engine material-editor renderer static-mesh-build asset-forge-builtins
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER MaterialEditor
	PRIVATE_SOURCE_RATIONALE
		"MaterialEditor-owned preview and panel white-box coverage avoids widening the editor module API."
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Exercises rendered material editing and preview lifecycle."
	SOURCES
		Private/Materials/MaterialSchemaAndEditingTests.cpp
		Private/Materials/MaterialGraphOperationsTests.cpp
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
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	LIBRARIES
		ApplicationCore
		RenderCore
		Renderer
		AssetTools
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
	MODULES asset-tools engine material-editor renderer static-mesh-build static-mesh-editor texture-build texture-editor
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
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	LIBRARIES
		ApplicationCore
		RenderCore
		Renderer
		AssetTools
		AssetForgeBuiltins
		MonaCore
		Mona
		MonaImGui
		DurinEd
		MaterialEditor
		StaticMeshEditor
		TextureEditor
		StaticMeshBuild
		TextureBuild
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(StaticMeshTests
	KIND feature
	DOMAINS static-mesh
	MODULES asset-tools engine static-mesh-build level-editor static-mesh-editor
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
	LIBRARIES AssetTools StaticMeshBuild TextureBuild AssetForgeBuiltins RenderCore Renderer DurinEd StaticMeshEditor
	INCLUDE_DIRECTORIES
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
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
	AssetTools
	Engine
	StaticMeshBuild
	TextureBuild
	AssetForgeBuiltins
	TextureEditor
	RenderCore
	Renderer
	DurinEd
)
