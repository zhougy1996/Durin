include(GoogleTest)

add_durin_test(EngineTests
	Private/MaterialTests.cpp
	Private/WorldTests.cpp
	Private/ViewportTests.cpp
	Private/EditorNotificationTests.cpp
	Private/UIStyleTests.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportCameraTransform.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Viewport/TransformGizmo.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/LevelEditorContext.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/LevelViewportSessionSettings.cpp
)

target_include_directories(EngineTests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private
)

target_link_libraries(EngineTests PRIVATE
	Core
	CoreDObject
	AssetCore
	Engine
	RenderCore
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
durin_test_deploy_target_binary(EngineTests DurinEd)
durin_test_deploy_runtime_files(EngineTests assimp::assimp)
durin_test_deploy_runtime_files(EngineTests Slang_Imported)

get_target_property(_durin_engine_tests_work_dir EngineTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(EngineTests
	WORKING_DIRECTORY "${_durin_engine_tests_work_dir}"
)
