durin_add_engine_functional_test(EnvironmentLightingTests
	KIND feature
	DOMAINS environment-lighting
	MODULES engine
	RUNTIME_STACK_RATIONALE "Exercises built-in environment payload and direct cook behavior."
	SOURCES Private/EnvironmentLightingTests.cpp
)

durin_add_engine_functional_test(RendererSceneContractTests
	KIND contract
	DOMAINS renderer
	MODULES engine renderer
	STACKS renderer
	RUNTIME_STACK_RATIONALE "Exercises renderer-owned SceneProxy and SceneInfo lifetime contracts."
	SOURCES Private/RendererSceneContractTests.cpp
	LIBRARIES RenderCore Renderer
)

durin_add_engine_functional_test(SceneImportVulkanTests
	KIND integration
	DOMAINS asset-import
	MODULES engine renderer standard-asset-import vulkan-rhi
	BACKENDS vulkan
	STACKS editor renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Owns the Vulkan-backed static-model import acceptance lifecycle."
	SOURCES Private/Texture/SceneImportVulkanTests.cpp
	LIBRARIES ApplicationCore AssetImportCore GeometryBuild StandardAssetImport RenderCore Renderer DurinEd TextureEditor VulkanRHI Vulkan::Vulkan bc7enc_rdo::bc7enc_rdo
	INCLUDE_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Source/Editor/StandardAssetImport/Private
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/VulkanRHI/Private
	COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)
durin_add_engine_functional_test(ThumbnailTests
	KIND feature
	DOMAINS thumbnail
	MODULES engine level-editor
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned thumbnail cache white-box coverage avoids exporting private cache implementations."
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises renderer-backed editor thumbnail generation and caching."
	SOURCES
		Private/SourceImageThumbnailTests.cpp
		Private/AssetThumbnailContractTests.cpp
		Private/RenderedAssetThumbnailFixtureTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Assets/SourceImageThumbnailCache.cpp
		${_durin_level_editor_private}/Assets/ContentBrowserThumbnailCache.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDecoder.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDiskCache.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd GeometryBuild StandardAssetImport StaticMeshEditor TextureEditor
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(MaterialThumbnailTests
	KIND feature
	DOMAINS material thumbnail
	MODULES engine material-editor
	STACKS editor renderer
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises the MaterialEditor-owned Material and MaterialInstance thumbnail extensions."
	SOURCES Private/MaterialAssetThumbnailTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd GeometryBuild StandardAssetImport MaterialEditor TextureEditor
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(TextureThumbnailTests
	KIND feature
	DOMAINS texture thumbnail
	MODULES engine texture-editor
	STACKS editor renderer
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises the TextureEditor-owned Texture2D and TextureCube thumbnail extensions."
	SOURCES Private/TextureAssetThumbnailTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd GeometryBuild StandardAssetImport TextureEditor
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)

durin_add_engine_functional_test(StaticMeshThumbnailTests
	KIND feature
	DOMAINS static-mesh thumbnail
	MODULES engine level-editor static-mesh-editor
	STACKS editor renderer
	PRIVATE_SOURCE_OWNER LevelEditor
	PRIVATE_SOURCE_RATIONALE
		"LevelEditor-owned thumbnail cache white-box coverage accompanies the StaticMeshEditor extension without exporting private symbols."
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises the StaticMeshEditor-owned thumbnail extension and cache lifecycle."
	SOURCES Private/StaticMeshAssetThumbnailTests.cpp
	PRIVATE_SOURCES
		${_durin_level_editor_private}/Assets/SourceImageThumbnailCache.cpp
		${_durin_level_editor_private}/Assets/ContentBrowserThumbnailCache.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDecoder.cpp
		${_durin_level_editor_private}/Assets/SourceImageThumbnailDiskCache.cpp
	LIBRARIES ApplicationCore RenderCore Renderer DurinEd GeometryBuild StandardAssetImport MaterialEditor StaticMeshEditor TextureEditor
	DATA_DIRECTORIES
		${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport
		${CMAKE_CURRENT_SOURCE_DIR}/Data
)
