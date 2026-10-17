durin_add_native_test(RoadGraphContractTests
	KIND contract
	DOMAINS road-graph road-surface
	MODULES roadweaver
	SOURCES Private/RoadGraphContractTests.cpp
	LIBRARIES RoadWeaver Engine
)

set(_road_scene_editor_libraries)
set(_road_scene_rationale)
if(DURIN_WITH_EDITOR)
	set(_road_scene_editor_libraries AssetForgeBuiltins TextureCompressor)
	set(_road_scene_rationale HEAVY_RUNTIME_RATIONALE "Loads the checked-in RoadWeaver level and its editor import metadata.")
endif()

durin_add_native_test(RoadSceneIntegrationTests
	KIND integration
	DOMAINS road-scene
	MODULES roadweaver
	SOURCES Private/RoadSceneIntegrationTests.cpp
	LIBRARIES RoadWeaver Engine MeshBuilder ${_road_scene_editor_libraries}
	${_road_scene_rationale}
)

durin_add_native_test(RoadEditorIntegrationTests
	KIND integration
	DOMAINS road-graph road-scene
	MODULES roadweaver durin-ed
	STACKS editor
	SOURCES Private/RoadEditorIntegrationTests.cpp
	LIBRARIES RoadWeaver Engine DurinEd
	REQUIRES editor
	REQUIREMENT_RATIONALE "Qualifies road structural edits through the editor property session and transaction buffer."
	HEAVY_RUNTIME_RATIONALE "Composes reflected road assets, generated previews and editor transaction history."
)

set(_road_level_editor_private "${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private")
durin_add_native_test(RoadViewportQualificationTests
	KIND qualification
	DOMAINS road-scene
	MODULES roadweaver level-editor renderer
	BACKENDS metal vulkan
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE "Exercises production click selection and GPU picking for generated road previews without exporting private viewport internals."
	SOURCES Private/RoadViewportQualificationTests.cpp
	PRIVATE_SOURCES
		${_road_level_editor_private}/Customizations/LevelEditorCustomizations.cpp
		${_road_level_editor_private}/Viewport/ViewportCameraTransform.cpp
		${_road_level_editor_private}/Viewport/LevelEditorViewportClient.cpp
		${_road_level_editor_private}/Viewport/ViewportPickingService.cpp
		${_road_level_editor_private}/Viewport/LevelEditorViewportEditing.cpp
		${_road_level_editor_private}/Viewport/TransformGizmo.cpp
		${_road_level_editor_private}/Workspace/LevelEditorContext.cpp
	LIBRARIES RoadWeaver Engine DurinEd Renderer MeshBuilder Mona MonaImGui
	INCLUDE_DIRECTORIES ${_road_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
	COMPILE_DEFINITIONS LEVELEDITOR_EXPORTS
	ENVIRONMENTS authored-shaders
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor selection, reflected road assets and authored materials."
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE "Qualifies generated road geometry, material pixels and native GPU hit-proxy readback."
)
