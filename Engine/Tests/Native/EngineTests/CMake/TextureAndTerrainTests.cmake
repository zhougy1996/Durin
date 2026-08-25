if(DURIN_WITH_EDITOR)
	add_durin_test(TextureTests
		Private/Texture/TextureTestEnvironment.cpp
		Private/Texture/TextureImportAndCacheTests.cpp
		Private/Texture/TextureDerivedDataTests.cpp
		Private/Texture/TextureBuildTests.cpp
		Private/Texture/VolumeTextureSourceImportTests.cpp
		Private/Texture/Texture2DBuildCoordinatorTests.cpp
		Private/Texture/TextureFailureTests.cpp
		Private/Texture/TextureSourceRelocationTests.cpp
		Private/Texture/SingleAssetImportTests.cpp
		Private/Texture/EquirectangularTextureCubeTests.cpp
		Private/TextureCubeTests.cpp
	)
	target_include_directories(TextureTests PRIVATE
		${_durin_texture_test_include_directories})
	target_link_libraries(TextureTests PRIVATE
		${_durin_texture_test_libraries})
	target_link_libraries(TextureTests PRIVATE bc7enc_rdo::bc7enc_rdo)
	set_target_properties(TextureTests PROPERTIES
		DURIN_TEST_HEAVY_RUNTIME_RATIONALE
			"Exercises editor texture import, build, cache, and render-resource contracts."
	)
	durin_test_deploy_directory_to_data(
		TextureTests
		"${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport"
	)
	durin_test_deploy_directory_to_data(
		TextureTests
		"${CMAKE_CURRENT_SOURCE_DIR}/Data"
	)
	durin_register_native_test(TextureTests
		KIND feature
		DOMAINS asset-workflow texture
		MODULES engine texture-build geometry-build terrain-build asset-forge asset-forge-builtins texture-editor
		STACKS editor
		TIMEOUT 600
	)

	add_durin_test(SceneImportTests
		Private/Texture/SceneImportTests.cpp
	)
	target_include_directories(SceneImportTests PRIVATE
		${_durin_texture_test_include_directories})
	target_link_libraries(SceneImportTests PRIVATE
		${_durin_texture_test_libraries}
		bc7enc_rdo::bc7enc_rdo)
	set_target_properties(SceneImportTests PROPERTIES
		DURIN_TEST_HEAVY_RUNTIME_RATIONALE
			"Exercises editor scene-import publication and rollback across runtime asset families."
	)
	durin_test_deploy_directory_to_data(
		SceneImportTests
		"${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport"
	)
	durin_register_native_test(SceneImportTests
		KIND integration
		DOMAINS asset-import
		MODULES engine asset-forge asset-forge-builtins
		STACKS editor
		TIMEOUT 600
	)
else()
	durin_exclude_native_test_sources(
		RATIONALE "Texture authoring and scene import require editor-only Build and AssetForgeBuiltins services."
		SOURCES
			Private/Texture/TextureTestEnvironment.cpp
			Private/Texture/TextureImportAndCacheTests.cpp
			Private/Texture/TextureDerivedDataTests.cpp
			Private/Texture/TextureBuildTests.cpp
			Private/Texture/VolumeTextureSourceImportTests.cpp
			Private/Texture/Texture2DBuildCoordinatorTests.cpp
			Private/Texture/TextureFailureTests.cpp
			Private/Texture/TextureSourceRelocationTests.cpp
			Private/Texture/SceneImportTests.cpp
			Private/Texture/SingleAssetImportTests.cpp
			Private/Texture/EquirectangularTextureCubeTests.cpp
			Private/TextureCubeTests.cpp
	)
endif()

durin_add_engine_functional_test(TerrainHeightmapTests
	KIND feature
	DOMAINS terrain
	MODULES engine terrain-build asset-forge asset-forge-builtins
	STACKS editor
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises heightmap import, DDC, package, and source-index integration."
	SOURCES Private/Terrain/TerrainHeightmapTests.cpp
	LIBRARIES AssetForge AssetForgeBuiltins DurinEd TerrainBuild
)

durin_add_engine_functional_test(TerrainHeightmapCookTests
	KIND integration
	DOMAINS asset-cook terrain
	MODULES engine terrain-build asset-forge asset-forge-builtins
	STACKS editor
	EDITOR_ONLY
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises source-free cooked heightmap package loading."
	SOURCES Private/Terrain/TerrainHeightmapCookTests.cpp
	LIBRARIES TerrainBuild AssetForgeBuiltins
)

durin_add_engine_functional_test(TerrainWorldBuildTests
	KIND integration
	DOMAINS asset-build asset-cook terrain
	MODULES terrain-build derived-data-cache asset-core
	STACKS editor
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises offline Terrain World tile codecs, build identities, generation publication, and Cook contracts."
	SOURCES Private/Terrain/TerrainWorldBuildTests.cpp
	LIBRARIES TerrainBuild DerivedDataCache
)

durin_add_engine_functional_test(TerrainRenderPrimitiveTests
	KIND contract
	DOMAINS terrain
	MODULES engine renderer
	STACKS renderer
	TIMEOUT 600
	RUNTIME_STACK_RATIONALE "Exercises reflected Terrain component, patch, proxy, and revision contracts."
	SOURCES Private/Terrain/TerrainRenderPrimitiveTests.cpp
	LIBRARIES Renderer RenderCore
)

durin_add_engine_functional_test(TerrainRenderVulkanTests
	KIND integration
	DOMAINS terrain
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE "Exercises exact R16 Terrain rendering, counters, and resource release on Vulkan."
	RUNTIME_ONLY_RATIONALE "RHIInit selects VulkanRHI dynamically for Terrain render validation."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/Terrain/TerrainRenderVulkanTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
)

durin_add_engine_functional_test(TerrainRenderQualificationTests
	KIND qualification
	DOMAINS terrain
	MODULES engine renderer
	BACKENDS vulkan
	STACKS renderer
	GPU
	TIMEOUT 900
	RUNTIME_STACK_RATIONALE
		"Measures maximum supported Terrain rendering on an initialized Vulkan device."
	RUNTIME_ONLY_RATIONALE
		"RHIInit selects VulkanRHI dynamically for Terrain render qualification."
	RUNTIME_ONLY_TARGETS VulkanRHI
	SOURCES Private/Terrain/TerrainRenderQualificationTests.cpp
	LIBRARIES ApplicationCore RenderCore Renderer
)
