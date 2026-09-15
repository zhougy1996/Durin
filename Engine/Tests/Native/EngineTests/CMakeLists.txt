include(GoogleTest)

set(_durin_level_editor_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Private")
set(_durin_material_editor_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Private")
set(_durin_static_mesh_editor_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/StaticMeshEditor/Private")
set(_durin_main_frame_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MainFrame/Private")
set(_durin_content_browser_public
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/ContentBrowser/Public")
set(_durin_content_browser_private
	"${CMAKE_SOURCE_DIR}/Engine/Source/Editor/ContentBrowser/Private")
# Preserve the existing macOS Vulkan admission policy during declaration migration.
set(_durin_vulkan_integration_kind integration)
if(APPLE)
	set(_durin_vulkan_integration_kind qualification)
endif()

# Keep domain fragments in registration order. Each target remains declared once,
# and registration stays after its complete metadata and dependency setup.
include("${CMAKE_CURRENT_LIST_DIR}/CMake/AssetAndMaterialTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CMake/TextureTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CMake/LaunchTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CMake/RenderingAndThumbnailTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CMake/WorldPhysicsAndViewportTests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CMake/SceneAndEditorTests.cmake")
