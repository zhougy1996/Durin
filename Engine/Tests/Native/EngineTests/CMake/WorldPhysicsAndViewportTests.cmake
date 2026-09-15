durin_add_native_test(InputActionTests
	KIND contract
	DOMAINS input gameplay
	MODULES engine mona mona-core mona-imgui
	SOURCES Private/Input/InputActionTests.cpp
	LIBRARIES Core CoreDObject Engine
)

set(_durin_world_test_sources
	Private/World/NewLevelBaselineTests.cpp
	Private/World/WorldPlayTests.cpp
	Private/World/WorldSubsystemTests.cpp
	Private/World/WorldLifecycleMutationTests.cpp
	Private/World/WorldTickSchedulingTests.cpp
	Private/World/WorldTimerTests.cpp
	Private/World/WorldActorIteratorTests.cpp
	Private/World/WorldActorTests.cpp
	Private/World/WorldComponentTests.cpp
	Private/World/WorldLifetimeTests.cpp
	Private/World/NativeGameplayCoreTests.cpp
)
durin_add_native_test(WorldTests
	REQUIRES editor
	REQUIREMENT_RATIONALE "WorldTests requires the DurinEd editor world lifecycle."
	KIND feature
	DOMAINS world
	MODULES engine durin-ed
	STACKS editor
	SOURCES ${_durin_world_test_sources}
	INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Private
	LIBRARIES Engine DurinEd AssetForgeBuiltins
	HEAVY_RUNTIME_RATIONALE "Exercises DurinEd world editing integration."
)

durin_add_native_test(PhysicsSceneTests
	KIND feature
	DOMAINS physics
	MODULES physics engine static-mesh-build
	STACKS editor
	SOURCES Private/Physics/PhysicsSceneTests.cpp Private/Physics/PhysicsQueryObservabilityTests.cpp
	LIBRARIES Core CoreDObject Engine PhysicsCore Physics StaticMeshBuild
	INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Private
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	HEAVY_RUNTIME_RATIONALE
		"Exercises editor-only StaticMesh collision-build registration through StaticMeshBuild."
)

durin_add_native_test(PhysicsQualificationTests
	KIND qualification
	DOMAINS physics
	MODULES physics engine
	SOURCES Private/Physics/PhysicsQualificationTests.cpp
	LIBRARIES Core CoreDObject Engine PhysicsCore Physics
)

durin_add_native_test(MonaCoreBoundaryTests
	KIND contract
	DOMAINS viewport
	MODULES mona-core
	SOURCES Private/Viewport/MonaCoreBoundaryTests.cpp
	LIBRARIES MonaCore
)

durin_add_native_test(MonaViewportTests
	KIND contract
	DOMAINS viewport
	MODULES mona
	SOURCES Private/Viewport/ViewportDisplaySourceTests.cpp
	INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Private
	LIBRARIES Core RHI MonaCore Mona
	HEAVY_RUNTIME_RATIONALE "Exercises the Mona display-source consumer without Engine linkage."
)

durin_add_native_test(EngineViewportHeaderTests
	KIND contract
	DOMAINS viewport
	MODULES engine
	SOURCES Private/Viewport/EngineViewportHeaderTests.cpp
	LIBRARIES Core Engine
)

set(_durin_viewport_test_sources
	Private/Viewport/SceneViewportResourceTests.cpp
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
	${_durin_level_editor_private}/Customizations/VolumetricCloudDetails.cpp
	${_durin_level_editor_private}/Settings/LevelViewportSessionSettings.cpp
	${_durin_level_editor_private}/Workspace/LevelEditorContext.cpp
)
if(DURIN_WITH_EDITOR)
	durin_add_native_test(ViewportTests
		KIND feature
		DOMAINS viewport
		MODULES engine level-editor mona static-mesh-build
		STACKS editor
		PRIVATE_SOURCE_OWNER LevelEditor
		PRIVATE_SOURCE_RATIONALE
			"LevelEditor-owned viewport white-box coverage without exporting private DLL symbols."
		TIMEOUT 600
		SOURCES ${_durin_viewport_test_sources} ${_durin_viewport_private_sources}
		INCLUDE_DIRECTORIES
			${CMAKE_CURRENT_SOURCE_DIR}/Private
			${_durin_level_editor_private}
			${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
			${CMAKE_SOURCE_DIR}/Engine/Source
			${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/MonaImGui/Private
		COMPILE_DEFINITIONS LEVELEDITOR_EXPORTS
		LIBRARIES
			Core
			CoreDObject
			RHI
			RenderCore
			Engine
			ApplicationCore
			MonaCore
			Mona
			MonaImGui
			AssetForgeBuiltins
			StaticMeshBuild
			DurinEd
		HEAVY_RUNTIME_RATIONALE "Exercises DurinEd and Mona viewport interaction behavior."
	)

	durin_add_native_test(ViewportQualificationTests
		KIND qualification
		DOMAINS viewport
		MODULES engine level-editor mona
		STACKS editor
		PRIVATE_SOURCE_OWNER LevelEditor
		PRIVATE_SOURCE_RATIONALE
			"LevelEditor-owned viewport qualification uses the same private picking seams as routine coverage."
		TIMEOUT 900
		SOURCES Private/Viewport/ViewportPickingQualificationTests.cpp ${_durin_viewport_private_sources}
		INCLUDE_DIRECTORIES
			${CMAKE_CURRENT_SOURCE_DIR}/Private
			${_durin_level_editor_private}
			${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
			${CMAKE_SOURCE_DIR}/Engine/Source
			${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/MonaImGui/Private
		COMPILE_DEFINITIONS LEVELEDITOR_EXPORTS
		LIBRARIES
			Core
			CoreDObject
			RHI
			RenderCore
			Engine
			ApplicationCore
			MonaCore
			Mona
			MonaImGui
			AssetForgeBuiltins
			StaticMeshBuild
			DurinEd
		HEAVY_RUNTIME_RATIONALE "Measures large-scale editor viewport picking behavior."
	)
else()
	durin_exclude_native_test_sources(
		RATIONALE "ViewportTests requires LevelEditor composition and DurinEd."
		SOURCES ${_durin_viewport_test_sources}
			Private/Viewport/ViewportPickingQualificationTests.cpp
	)
endif()
