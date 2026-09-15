durin_add_native_test(AssetSaveReadinessTests
	KIND contract
	DOMAINS asset-workflow
	MODULES engine
	SOURCES Private/AssetSaveReadinessTests.cpp
	LIBRARIES Core CoreDObject Engine
)

durin_add_native_test(AssetCompilingManagerTests
	KIND contract
	DOMAINS asset-workflow
	MODULES engine
	SOURCES Private/AssetCompilingManagerTests.cpp
	LIBRARIES Core CoreDObject Engine
)

durin_add_native_test(CookedMeshLoadingTests
	KIND contract
	DOMAINS asset-workflow static-mesh
	MODULES engine
	SOURCES Private/CookedMeshLoadManagerTests.cpp
	LIBRARIES Core CoreDObject Engine
)

durin_add_native_test(DerivedDataCacheTests
	KIND contract
	DOMAINS derived-data
	MODULES derived-data-cache
	SOURCES Private/DerivedDataCacheTests.cpp
	LIBRARIES Core CoreDObject Engine DerivedDataCache
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE "Exercises the Developer-only derived-data cache contract."
)

durin_add_native_test(EditorPropertyTests
	KIND feature
	DOMAINS property-editor
	MODULES durin-ed level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned property customization white-box coverage avoids exporting editor implementation symbols."
	SOURCES
		Private/ReflectedPropertyViewTests.cpp
		Private/Editor/ReflectedPropertyEditSessionTests.cpp
		Private/Editor/ReflectedPropertyTransactionTests.cpp
		Private/Editor/ReflectedPropertyContainerTests.cpp
	PRIVATE_SOURCES ${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	LIBRARIES Core CoreDObject Engine DurinEd
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
	COMPILE_DEFINITIONS LEVELEDITOR_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE "Exercises DurinEd property customization behavior."
)

durin_add_native_test(EditorOperationTests
	KIND contract
	DOMAINS editor-operation
	MODULES asset-tools durin-ed
	STACKS editor
	SOURCES
		Private/Editor/CompensatingAsyncOperationTests.cpp
		Private/Editor/FactoryTests.cpp
		Private/Editor/TransactionRecordTests.cpp
	LIBRARIES Core CoreDObject Engine AssetTools DurinEd
	INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Private
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE
		"Exercises reusable DurinEd asynchronous operation orchestration without an application host."
)

durin_add_native_test(EditorAssetWorkflowTests
	KIND feature
	DOMAINS asset-workflow
	MODULES asset-maintenance asset-tools content-browser durin-ed main-frame texture-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER TextureEditor
	PRIVATE_SOURCE_RATIONALE
		"TextureEditor-owned import forms and browser detail cache remain private while their state and metadata inspection are white-box tested."
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
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_main_frame_private}
		${_durin_content_browser_public}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/TextureEditor/Private
	LIBRARIES
		Core
		CoreDObject
		Engine
		ApplicationCore
		MonaCore
		Mona
		MonaImGui
		AssetMaintenance
		AssetTools
		ContentBrowser
		DurinEd
		MainFrame
		StaticMeshBuild
		AssetForgeBuiltins
		bc7enc_rdo::bc7enc_rdo
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	COMPILE_DEFINITIONS TEXTUREEDITOR_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE "Exercises editor asset workflows across DurinEd and Mona UI models."
)

durin_add_native_test(ContentBrowserWorkflowTests
	KIND feature
	DOMAINS asset-workflow
	MODULES content-browser
	STACKS editor
	PRIVATE_SOURCE_OWNER ContentBrowser
	PRIVATE_SOURCE_RATIONALE
		"ContentBrowser-owned workflow white-box coverage avoids exporting model and operation implementations."
	SOURCES
		Private/Editor/ContentBrowserExtensionRegistryTests.cpp
		Private/Editor/ContentBrowserPanelExtensionTests.cpp
		Private/Editor/ContentBrowserItemViewTests.cpp
		Private/Editor/ContentBrowserModelTests.cpp
		Private/Editor/ContentBrowserRefreshCoordinatorTests.cpp
	PRIVATE_SOURCES
		${_durin_content_browser_private}/ContentBrowserExtensionRegistry.cpp
		${_durin_content_browser_private}/Panels/AssetCreationDialog.cpp
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
	LIBRARIES
		Core
		CoreDObject
		Engine
		ApplicationCore
		MonaCore
		Mona
		MonaImGui
		DurinEd
		AssetTools
		StaticMeshBuild
		TextureBuild
		AssetForgeBuiltins
		bc7enc_rdo::bc7enc_rdo
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_content_browser_public}
		${_durin_content_browser_private}
	COMPILE_DEFINITIONS CONTENTBROWSER_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE
		"Exercises ContentBrowser asset workflows across Engine Asset, DurinEd, and Mona UI models."
)

durin_add_native_test(AssetReferenceStoreTests
	KIND contract
	DOMAINS asset-reference
	MODULES level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned project reference-store white-box coverage avoids exporting private settings symbols."
	SOURCES Private/Editor/ProjectDefaultLevelReferenceStoreTests.cpp Private/Editor/ProjectGameSettingsTests.cpp
	PRIVATE_SOURCES ${_durin_level_editor_private}/Settings/ProjectDefaultLevelReferenceStore.cpp
	INCLUDE_DIRECTORIES ${_durin_level_editor_private}
	COMPILE_DEFINITIONS LEVELEDITOR_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE "Exercises production external asset-reference stores."
	LIBRARIES Core CoreDObject Engine
)

durin_add_native_test(EditorHierarchyTests
	KIND feature
	DOMAINS hierarchy
	MODULES level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned hierarchy model white-box coverage avoids exporting its private model implementation."
	SOURCES Private/Editor/WorldOutlinerHierarchyModelTests.cpp
	PRIVATE_SOURCES ${_durin_level_editor_private}/Panels/WorldOutlinerHierarchyModel.cpp
	INCLUDE_DIRECTORIES ${_durin_level_editor_private}
	COMPILE_DEFINITIONS LEVELEDITOR_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE "Exercises the deterministic Level Editor hierarchy model without editor startup."
	LIBRARIES Core CoreDObject Engine
)

durin_add_native_test(LevelMutationTests
	KIND feature
	DOMAINS world
	MODULES durin-ed level-editor
	STACKS editor
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned mutation white-box coverage keeps structural transaction implementations private."
	COMPILE_DEFINITIONS LEVELEDITOR_EXPORTS DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION=1
	SOURCES Private/Editor/StaticMeshLevelMutationTests.cpp Private/Editor/WorldOutlinerActorAttachmentTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Operations/StaticMeshLevelMutations.cpp
		${_durin_level_editor_private}/Panels/ActorAttachmentTransaction.cpp
	LIBRARIES Core CoreDObject Engine DurinEd
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE "Exercises transaction-backed LevelEditor structural mutation."
)

# Shared material domains select whole targets; each suite and native source has one owner.
durin_add_native_test(MaterialCompilerTests
	KIND feature
	DOMAINS material material-compiler
	MODULES engine material-editor renderer asset-tools asset-forge-builtins static-mesh-build
	STACKS editor renderer
	TIMEOUT 300
	SOURCES Private/Materials/MaterialCompilerTests.cpp Private/Materials/MaterialExpressionTests.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	LIBRARIES
		Core
		CoreDObject
		Engine
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
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE
		"Exercises typed expression validation, detached IR normalization, and complete shader compilation."
)

durin_add_native_test(MaterialFunctionTests
	KIND feature
	DOMAINS material material-function
	MODULES engine material-editor renderer asset-tools asset-forge-builtins static-mesh-build
	STACKS editor renderer
	TIMEOUT 300
	SOURCES Private/Materials/MaterialFunctionTests.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	LIBRARIES
		Core
		CoreDObject
		Engine
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
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE
		"Exercises authored material-function recipes, expansion, dependencies, and interface contracts."
)

durin_add_native_test(MaterialEditingTests
	KIND feature
	DOMAINS material material-editing
	MODULES engine material-editor renderer asset-tools asset-forge-builtins static-mesh-build
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER MaterialEditor
	PRIVATE_SOURCE_RATIONALE "Exercises MaterialEditor-owned private widgets without exporting test-only APIs."
	TIMEOUT 300
	SOURCES
		Private/Materials/MaterialPropertyEditingTests.cpp
		Private/Materials/MaterialGraphOperationsTests.cpp
		Private/Materials/MaterialEditingSessionTests.cpp
		Private/Materials/MaterialFunctionEditingTests.cpp
		Private/Materials/MaterialPreviewTests.cpp
		Private/MaterialParameterPanelModelTests.cpp
	PRIVATE_SOURCES
		${_durin_material_editor_private}/Graph/MaterialGraphCanvas.cpp
		${_durin_material_editor_private}/Graph/MaterialGraphTexturePreviews.cpp
		${_durin_material_editor_private}/Graph/MaterialGraphInputDetails.cpp
		${_durin_material_editor_private}/Graph/MaterialGraphCreationMenu.cpp
		${_durin_material_editor_private}/Widgets/MaterialPreview.cpp
		${_durin_material_editor_private}/Widgets/MaterialEditingSession.cpp
		${_durin_material_editor_private}/Widgets/MaterialParameterPanelModel.cpp
		${_durin_material_editor_private}/Widgets/MMaterialEditor.cpp
		${_durin_material_editor_private}/Widgets/MMaterialFunctionEditor.cpp
		${_durin_material_editor_private}/Widgets/MaterialFunctionCallPicker.cpp
		${_durin_material_editor_private}/Settings/MaterialEditorSessionSettings.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/StaticMeshEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	LIBRARIES
		Core
		CoreDObject
		Engine
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
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	COMPILE_DEFINITIONS MATERIALEDITOR_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE
		"Exercises graph authoring, transactions, private widgets, and editor preview ownership."
)

durin_add_native_test(MaterialRuntimeTests
	KIND feature
	DOMAINS material material-runtime
	MODULES engine material-editor renderer asset-tools asset-forge-builtins static-mesh-build
	STACKS editor renderer
	TIMEOUT 300
	SOURCES
		Private/Materials/MaterialDependencyTests.cpp
		Private/Materials/MaterialRenderProxyTests.cpp
		Private/Materials/MaterialInstanceTests.cpp
		Private/Materials/MaterialRenderingTests.cpp
		Private/Materials/MaterialRenderRepresentationTests.cpp
		Private/Materials/MaterialProgramPublicationTests.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/StaticMeshEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	LIBRARIES
		Core
		CoreDObject
		Engine
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
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE
		"Exercises material inheritance, publication, render binding, and CPU scene integration."
)

durin_add_native_test(MaterialCompileLifecycleTests
	KIND feature
	DOMAINS material material-compilation
	MODULES engine material-editor renderer asset-tools asset-forge-builtins static-mesh-build
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER MaterialEditor
	PRIVATE_SOURCE_RATIONALE "Exercises MaterialEditor-owned private widgets without exporting test-only APIs."
	TIMEOUT 300
	SOURCES
		Private/Materials/MaterialCompileLifecycleTests.cpp
		Private/Materials/MaterialAsyncEditingSessionTestSupport.cpp
	PRIVATE_SOURCES ${_durin_material_editor_private}/Widgets/MaterialEditingSession.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	LIBRARIES
		Core
		CoreDObject
		Engine
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
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	COMPILE_DEFINITIONS MATERIALEDITOR_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE
		"Exercises asynchronous compiler scheduling, owner cancellation, editor apply, and shutdown."
)

durin_add_native_test(MaterialCookTests
	KIND feature
	DOMAINS material material-cook
	MODULES engine material-editor renderer asset-tools asset-forge-builtins static-mesh-build
	STACKS editor renderer
	TIMEOUT 300
	SOURCES Private/Materials/MaterialCookTests.cpp Private/Materials/MaterialFunctionCookTests.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	LIBRARIES
		Core
		CoreDObject
		Engine
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
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE
		"Exercises material cooking, function dependency fingerprints, and cooked-only loading."
)

durin_add_native_test(MaterialPackageTests
	KIND feature
	DOMAINS material material-package
	MODULES engine material-editor renderer asset-tools asset-forge-builtins static-mesh-build
	STACKS editor renderer
	TIMEOUT 300
	SOURCES Private/Materials/MaterialPackageTests.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	LIBRARIES
		Core
		CoreDObject
		Engine
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
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE
		"Exercises authored material package persistence, texture dependencies, and schema rejection."
)

durin_add_native_test(MaterialQualificationTests
	KIND qualification
	DOMAINS material
	MODULES engine material-editor renderer
	STACKS editor renderer
	TIMEOUT 900
	SOURCES Private/Materials/MaterialQualificationTests.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	LIBRARIES
		Core
		CoreDObject
		Engine
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
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE "Measures CPU material graph loading, layout, and shader compilation baselines."
)

durin_add_native_test(StaticMeshTests
	KIND feature
	DOMAINS static-mesh
	MODULES asset-tools engine static-mesh-build level-editor static-mesh-editor
	STACKS editor renderer
	TIMEOUT 600
	SOURCES
		Private/Materials/StaticMeshImportTests.cpp
		Private/Materials/StaticMeshRenderDataLifetimeContractTests.cpp
		Private/Materials/StaticMeshUpdateTests.cpp
		Private/StaticMeshTestEnvironment.cpp
		Private/StaticMeshDerivedDataContractTests.cpp
		Private/StaticMeshDerivedDataCacheTests.cpp
		Private/StaticMeshPayloadCodecTests.cpp
		Private/StaticMeshCollisionRoutineTests.cpp
		Private/StaticMeshEditorTests.cpp
	LIBRARIES
		Core
		CoreDObject
		Engine
		AssetTools
		StaticMeshBuild
		TextureBuild
		AssetForgeBuiltins
		RenderCore
		Renderer
		DurinEd
		StaticMeshEditor
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${_durin_static_mesh_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/StaticMeshEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE "Exercises renderer-backed static-mesh editing and derived data."
)

durin_add_native_test(StaticMeshMaterialTests
	KIND feature
	DOMAINS material material-binding static-mesh
	MODULES asset-tools engine static-mesh-build level-editor static-mesh-editor
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"Exercises LevelEditor-owned material slot customization without exporting private symbols."
	TIMEOUT 300
	SOURCES Private/Materials/StaticMeshMaterialTests.cpp Private/StaticMeshMaterialSlotDetailsTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Customizations/StaticMeshMaterialSlotDetails.cpp
		${_durin_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
	LIBRARIES
		Core
		CoreDObject
		Engine
		AssetTools
		StaticMeshBuild
		TextureBuild
		AssetForgeBuiltins
		RenderCore
		Renderer
		DurinEd
		StaticMeshEditor
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
	COMPILE_DEFINITIONS LEVELEDITOR_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE
		"Exercises imported mesh material slots, component bindings, and material slot editing."
)

durin_add_native_test(StaticMeshBuildQualificationTests
	KIND qualification
	DOMAINS static-mesh
	MODULES engine static-mesh-build
	STACKS renderer
	TIMEOUT 600
	SOURCES Private/StaticMeshBuildQualificationTests.cpp
	LIBRARIES Core CoreDObject Engine StaticMeshBuild TextureBuild AssetForgeBuiltins RenderCore Renderer
	INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Private
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE
		"Measures large authored static-mesh build, publication, cancellation, and residency costs."
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
