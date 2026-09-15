durin_add_native_test(RendererSceneContractTests
	KIND contract
	DOMAINS renderer
	MODULES engine renderer
	STACKS renderer
	SOURCES Private/RendererSceneContractTests.cpp
	LIBRARIES Core CoreDObject Engine RenderCore Renderer
	COMPILE_DEFINITIONS DURIN_RENDERER_SOURCE_DIR="${DURIN_PROJECT_SOURCE_DIR}/Runtime/Renderer"
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE "Exercises renderer-owned SceneProxy and SceneInfo lifetime contracts."
)

durin_add_native_test(SceneImportVulkanTests
	KIND ${_durin_vulkan_integration_kind}
	DOMAINS asset-import
	MODULES asset-tools engine renderer asset-forge-builtins vulkan-rhi
	BACKENDS vulkan
	STACKS editor renderer
	TIMEOUT 900
	SOURCES Private/Texture/SceneImportVulkanTests.cpp
	LIBRARIES
		Core
		CoreDObject
		Engine
		ApplicationCore
		AssetTools
		StaticMeshBuild
		AssetForgeBuiltins
		RenderCore
		Renderer
		DurinEd
		TextureEditor
		VulkanRHI
		Vulkan::Vulkan
		bc7enc_rdo::bc7enc_rdo
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/StaticMeshEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${DURIN_PROJECT_ROOT_DIR}/Source/Editor/AssetForgeBuiltins/Private
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
	COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE "Owns the Vulkan-backed static-model import acceptance lifecycle."
)
durin_add_native_test(ThumbnailTests
	KIND feature
	DOMAINS thumbnail
	MODULES engine content-browser
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER ContentBrowser
	PRIVATE_SOURCE_RATIONALE
		"ContentBrowser-owned thumbnail cache white-box coverage avoids exporting private cache implementations."
	TIMEOUT 600
	SOURCES
		Private/SourceImageThumbnailTests.cpp
		Private/AssetThumbnailContractTests.cpp
		Private/AssetThumbnailFixtureTests.cpp
	PRIVATE_SOURCES
		${_durin_content_browser_private}/Assets/SourceImageThumbnailCache.cpp
		${_durin_content_browser_private}/Assets/ContentBrowserThumbnailReferences.cpp
		${_durin_content_browser_private}/Assets/SourceImageThumbnailDecoder.cpp
		${_durin_content_browser_private}/Assets/SourceImageThumbnailDiskCache.cpp
	LIBRARIES
		Core
		CoreDObject
		Engine
		ApplicationCore
		RenderCore
		Renderer
		DurinEd
		AssetTools
		StaticMeshBuild
		AssetForgeBuiltins
		StaticMeshEditor
		TextureEditor
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/StaticMeshEditor/Public
		${_durin_content_browser_private}
		${CMAKE_SOURCE_DIR}/Engine/Source
	COMPILE_DEFINITIONS CONTENTBROWSER_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE "Exercises renderer-backed editor thumbnail generation and caching."
)

durin_add_native_test(MaterialThumbnailTests
	KIND feature
	DOMAINS material thumbnail
	MODULES engine material-editor
	STACKS editor renderer
	TIMEOUT 600
	SOURCES Private/MaterialThumbnailRendererTests.cpp
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
		DurinEd
		AssetTools
		StaticMeshBuild
		AssetForgeBuiltins
		MaterialEditor
		TextureEditor
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE
		"Exercises the MaterialEditor-owned Material and MaterialInstance thumbnail extensions."
)

durin_add_native_test(TextureThumbnailTests
	KIND feature
	DOMAINS texture thumbnail
	MODULES engine texture-build texture-editor
	STACKS editor renderer
	TIMEOUT 600
	SOURCES Private/TextureAssetThumbnailTests.cpp
	LIBRARIES
		Core
		CoreDObject
		Engine
		ApplicationCore
		RenderCore
		Renderer
		DurinEd
		AssetTools
		StaticMeshBuild
		TextureBuild
		AssetForgeBuiltins
		TextureEditor
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/StaticMeshEditor/Public
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE "Exercises the TextureEditor-owned Texture2D and TextureCube thumbnail extensions."
)

durin_add_native_test(StaticMeshThumbnailTests
	KIND feature
	DOMAINS static-mesh thumbnail
	MODULES engine content-browser static-mesh-editor
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER ContentBrowser
	PRIVATE_SOURCE_RATIONALE
		"ContentBrowser-owned thumbnail cache white-box coverage accompanies the StaticMeshEditor extension without exporting private symbols."
	TIMEOUT 600
	SOURCES Private/StaticMeshThumbnailRendererTests.cpp
	PRIVATE_SOURCES
		${_durin_content_browser_private}/Assets/SourceImageThumbnailCache.cpp
		${_durin_content_browser_private}/Assets/ContentBrowserThumbnailReferences.cpp
		${_durin_content_browser_private}/Assets/SourceImageThumbnailDecoder.cpp
		${_durin_content_browser_private}/Assets/SourceImageThumbnailDiskCache.cpp
	LIBRARIES
		Core
		CoreDObject
		Engine
		ApplicationCore
		RenderCore
		Renderer
		DurinEd
		AssetTools
		StaticMeshBuild
		AssetForgeBuiltins
		MaterialEditor
		StaticMeshEditor
		TextureEditor
	DATA_DIRECTORIES ${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport ${CMAKE_CURRENT_SOURCE_DIR}/Data
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/StaticMeshEditor/Public
		${_durin_content_browser_private}
	COMPILE_DEFINITIONS CONTENTBROWSER_EXPORTS
	REQUIRES editor
	REQUIREMENT_RATIONALE "Uses editor-only build services or editor module implementations."
	ENVIRONMENTS authored-shaders
	HEAVY_RUNTIME_RATIONALE "Exercises the StaticMeshEditor-owned thumbnail extension and cache lifecycle."
)

durin_add_native_test(ThumbnailVulkanTests
	KIND ${_durin_vulkan_integration_kind}
	DOMAINS material thumbnail
	MODULES
		asset-tools
		engine
		material-editor
		renderer
		static-mesh-build
		static-mesh-editor
		texture-build
		texture-editor
		vulkan-rhi
	BACKENDS vulkan
	STACKS editor renderer
	TIMEOUT 900
	SOURCES Private/ThumbnailVulkanTests.cpp
	INCLUDE_DIRECTORIES
		${CMAKE_CURRENT_SOURCE_DIR}/Private
		${_durin_level_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/LevelEditor/Public
		${_durin_material_editor_private}
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MaterialEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Editor/StaticMeshEditor/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
	COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
	LIBRARIES
		Core
		CoreDObject
		Engine
		VulkanRHI
		Vulkan::Vulkan
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
	RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
	HEAVY_RUNTIME_RATIONALE
		"Exercises thumbnail generation, disk readback, resource revision recovery, and cancellation on Vulkan."
)
