include(GoogleTest)

add_durin_test(EngineTests
	Private/DetailsPropertyEditingTests.cpp
	Private/MaterialTests.cpp
	Private/TextureTests.cpp
	Private/WorldTests.cpp
	Private/ViewportTests.cpp
	Private/EditorNotificationTests.cpp
	Private/UIStyleTests.cpp
	Private/SourceImageThumbnailTests.cpp
	Private/SplineTests.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/SourceImageThumbnailDecoder.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportCameraTransform.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Viewport/CameraPreviewViewportClient.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Viewport/TransformGizmo.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/CameraEditorCustomizations.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/DirectionalLightEditorCustomizations.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/SplineEditorCustomizations.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/LevelEditorCustomizations.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/LevelEditorContext.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/LevelViewportSessionSettings.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPropertyEditing.cpp
)

target_include_directories(EngineTests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
	${CMAKE_SOURCE_DIR}/Engine/Source
)

# The test target compiles the lightweight LevelEditor customization registry directly.
target_compile_definitions(EngineTests PRIVATE LEVELEDITOR_EXPORTS)

target_link_libraries(EngineTests PRIVATE
	Core
	CoreDObject
	AssetCore
	Engine
	RenderCore
	Renderer
	MonaCore
	Mona
	MonaImGui
	DurinEd
	GTest::gtest_main
)

durin_test_deploy_target_binary(EngineTests Core)
durin_test_deploy_target_binary(EngineTests CoreDObject)
durin_test_deploy_target_binary(EngineTests Engine)
durin_test_deploy_target_binary(EngineTests ApplicationCore)
durin_test_deploy_target_binary(EngineTests AssetCore)
durin_test_deploy_target_binary(EngineTests MonaCore)
durin_test_deploy_target_binary(EngineTests Mona)
durin_test_deploy_target_binary(EngineTests MonaImGui)
durin_test_deploy_target_binary(EngineTests RHI)
durin_test_deploy_target_binary(EngineTests RenderCore)
durin_test_deploy_target_binary(EngineTests Renderer)
durin_test_deploy_target_binary(EngineTests DurinEd)
durin_test_deploy_runtime_files(EngineTests assimp::assimp)
durin_test_deploy_runtime_files(EngineTests Slang_Imported)
durin_test_deploy_directory_to_data(EngineTests "${CMAKE_SOURCE_DIR}/Engine/Source/Programs/Tests/AssetCoreTests/TestData")

get_target_property(_durin_engine_tests_work_dir EngineTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(EngineTests
	WORKING_DIRECTORY "${_durin_engine_tests_work_dir}"
)
