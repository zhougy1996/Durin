durin_add_native_test(TextureTests
	REQUIRES editor
	REQUIREMENT_RATIONALE
		"Texture processing and scene import require TextureBuild and AssetForgeBuiltins editor services."
	KIND feature
	DOMAINS asset-workflow texture
	MODULES asset-tools engine texture-build static-mesh-build asset-forge-builtins texture-editor
	STACKS editor
	TIMEOUT 600
	SOURCES
		Private/Texture/TextureTestEnvironment.cpp
		Private/Texture/TextureImportAndCacheTests.cpp
		Private/Texture/TextureDerivedDataTests.cpp
		Private/Texture/TextureBuildTests.cpp
		Private/Texture/VolumeTextureSourceImportTests.cpp
		Private/Texture/TextureFailureTests.cpp
		Private/Texture/TextureCookedBaseStateTests.cpp
		Private/Texture/SingleAssetImportTests.cpp
		Private/Texture/EquirectangularTextureCubeTests.cpp
		Private/TextureCubeTests.cpp
	INCLUDE_DIRECTORIES ${_durin_texture_test_include_directories}
	LIBRARIES ${_durin_texture_test_libraries} bc7enc_rdo::bc7enc_rdo
	HEAVY_RUNTIME_RATIONALE "Exercises editor texture import, build, cache, and render-resource contracts."
	DATA_DIRECTORIES "${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport" "${CMAKE_CURRENT_SOURCE_DIR}/Data"
)

durin_add_native_test(SceneImportTests
	REQUIRES editor
	REQUIREMENT_RATIONALE
		"Texture processing and scene import require TextureBuild and AssetForgeBuiltins editor services."
	KIND integration
	DOMAINS asset-import
	MODULES engine texture-build asset-forge-builtins
	STACKS editor
	TIMEOUT 600
	SOURCES Private/Texture/SceneImportTests.cpp
	INCLUDE_DIRECTORIES ${_durin_texture_test_include_directories}
	LIBRARIES ShaderBuild ${_durin_texture_test_libraries} TextureBuild bc7enc_rdo::bc7enc_rdo
	HEAVY_RUNTIME_RATIONALE
		"Exercises editor scene-import publication and rollback across runtime asset families."
	DATA_DIRECTORIES "${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport"
)
