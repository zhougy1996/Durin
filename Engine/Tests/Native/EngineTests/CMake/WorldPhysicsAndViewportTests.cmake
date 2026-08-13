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
	MODULES aether engine geometry-build
	STACKS editor
	RUNTIME_STACK_RATIONALE
		"Exercises editor-only StaticMesh collision authoring registration through GeometryBuild."
	SOURCES
		Private/Physics/PhysicsSceneTests.cpp
		Private/Physics/PhysicsQueryObservabilityTests.cpp
	LIBRARIES AetherCore Aether GeometryBuild
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
	${_durin_level_editor_private}/Customizations/TerrainDetails.cpp
	${_durin_level_editor_private}/Assets/TerrainHeightmapAssetThumbnail.cpp
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
		GeometryBuild
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
		GeometryBuild
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
