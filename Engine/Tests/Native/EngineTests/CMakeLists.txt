include(GoogleTest)

add_durin_test(EngineTests
	Private/ReflectedPropertyViewTests.cpp
	Private/Editor/ReflectedPropertyEditSessionTests.cpp
	Private/Editor/ReflectedPropertyTransactionTests.cpp
	Private/Editor/ReflectedPropertyContainerTests.cpp
	Private/Materials/MaterialSchemaAndEditingTests.cpp
	Private/Materials/MaterialDependencyTests.cpp
	Private/Materials/MaterialUpdateContextTests.cpp
	Private/Materials/MaterialInstanceTests.cpp
	Private/Materials/MaterialRenderingTests.cpp
	Private/Materials/StaticMeshMaterialTests.cpp
	Private/Materials/StaticMeshUpdateTests.cpp
	Private/StaticMeshDerivedDataContractTests.cpp
	Private/MaterialParameterPanelModelTests.cpp
	Private/StaticMeshMaterialSlotDetailsTests.cpp
	Private/Texture/TextureImportAndCacheTests.cpp
	Private/Texture/TextureBuildTests.cpp
	Private/Texture/TextureFailureTests.cpp
	Private/TextureCubeTests.cpp
	Private/SkyBox/SkyBoxRenderingTests.cpp
	Private/SkyBox/SkyBoxComponentTests.cpp
	Private/SkyBox/SkyBoxEditorTests.cpp
	Private/SkyBox/SkyBoxVulkanTests.cpp
	Private/World/WorldPlayTests.cpp
	Private/World/WorldActorTests.cpp
	Private/World/WorldComponentTests.cpp
	Private/World/WorldLifetimeTests.cpp
	Private/Viewport/ViewportFoundationTests.cpp
	Private/Viewport/ViewportProjectionTests.cpp
	Private/Viewport/ViewportCustomizationTests.cpp
	Private/Viewport/ViewportInteractionTests.cpp
	Private/EditorGridRenderingTests.cpp
	Private/RendererEditorAssistanceTests.cpp
	Private/RendererRenderTargetLayoutTests.cpp
	Private/EditorNotificationTests.cpp
	Private/EditorWorkspaceTests.cpp
	Private/EditorTextureSmokeTests.cpp
	Private/UIStyleTests.cpp
	Private/ConsoleRecordModelTests.cpp
	Private/SourceImageThumbnailTests.cpp
	Private/AssetThumbnailContractTests.cpp
	Private/RenderedAssetThumbnailFixtureTests.cpp
	Private/SplineTests.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailCache.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailDecoder.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailDiskCache.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportCameraTransform.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Viewport/CameraPreviewViewportClient.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Viewport/TransformGizmo.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Customizations/CameraEditorCustomizations.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Customizations/DirectionalLightEditorCustomizations.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Customizations/SplineEditorCustomizations.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Customizations/LevelEditorCustomizations.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Customizations/ObjectPropertyEditorCustomizations.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Customizations/StaticMeshMaterialSlotDetails.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Customizations/SkyBoxDetails.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Workspace/LevelEditorContext.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Settings/LevelViewportSessionSettings.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private/Panels/ConsoleRecordModel.cpp
	# Compile the preview implementation directly so its private GC lifetime contract can be exercised.
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Private/Widgets/MaterialPreview.cpp
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Private/Widgets/MaterialParameterPanelModel.cpp
)

target_include_directories(EngineTests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/Private
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Private
	${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
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
	bc7enc_rdo::bc7enc_rdo
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
durin_test_deploy_target_binary(EngineTests VulkanRHI)
durin_test_deploy_target_binary(EngineTests DurinEd)
durin_test_deploy_runtime_files(EngineTests assimp::assimp)
durin_test_deploy_runtime_files(EngineTests Slang_Imported)
durin_test_deploy_directory_to_data(EngineTests "${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport")
durin_test_deploy_directory_to_data(EngineTests "${CMAKE_CURRENT_SOURCE_DIR}/Data")

get_target_property(_durin_engine_tests_work_dir EngineTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(EngineTests
	WORKING_DIRECTORY "${_durin_engine_tests_work_dir}"
)
