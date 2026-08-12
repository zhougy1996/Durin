add_library(LevelEditorViewportContracts OBJECT
	Internal/ViewportContracts/CameraEditorCustomizations.cpp
	Internal/ViewportContracts/CameraPreviewViewportClient.cpp
	Internal/ViewportContracts/DetailsPanelTargeting.cpp
	Internal/ViewportContracts/DirectionalLightEditorCustomizations.cpp
	Internal/ViewportContracts/LevelEditorContext.cpp
	Internal/ViewportContracts/LevelEditorCustomizations.cpp
	Internal/ViewportContracts/LevelEditorViewportClient.cpp
	Internal/ViewportContracts/LevelEditorViewportEditing.cpp
	Internal/ViewportContracts/LevelViewportSessionSettings.cpp
	Internal/ViewportContracts/PlayerStartEditorCustomizations.cpp
	Internal/ViewportContracts/SplineEditorCustomizations.cpp
	Internal/ViewportContracts/TransformGizmo.cpp
	Internal/ViewportContracts/ViewportCameraTransform.cpp
	Internal/ViewportContracts/ViewportPickingSceneIndex.cpp
	Internal/ViewportContracts/ViewportPickingService.cpp
)
durin_target_apply_common_definitions(
	LevelEditorViewportContracts
	LevelEditorViewportContracts
)
target_compile_definitions(
	LevelEditorViewportContracts
	PRIVATE LEVELEDITOR_EXPORTS
)
target_include_directories(LevelEditorViewportContracts PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/Public
	${CMAKE_CURRENT_SOURCE_DIR}/Private
	${CMAKE_SOURCE_DIR}/Engine/Source
)
target_link_libraries(LevelEditorViewportContracts PUBLIC
	Core
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
target_precompile_headers(LevelEditorViewportContracts PRIVATE
	"$<$<COMPILE_LANGUAGE:CXX>:${DURIN_PROJECT_ROOT_DIR}/CMake/SharedPCH/SharedPCH_MonaEditor.h>"
)
set_target_properties(LevelEditorViewportContracts PROPERTIES
	FOLDER "Engine/LevelEditor"
	POSITION_INDEPENDENT_CODE TRUE
)

add_durin_module(LevelEditor)
target_link_libraries(LevelEditor PRIVATE LevelEditorViewportContracts)

target_include_directories(LevelEditor PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Source
)
