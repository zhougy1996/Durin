durin_add_native_test(TextureTests
	REQUIRES editor
	REQUIREMENT_RATIONALE
		"Texture processing and scene import require TextureCompressor and AssetForgeBuiltins editor services."
	KIND feature
	DOMAINS asset-workflow texture
	MODULES asset-tools engine texture-compressor static-mesh-build asset-forge-builtins texture-editor
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
	LIBRARIES ${_durin_texture_test_libraries} DerivedDataCache bc7enc_rdo::bc7enc_rdo
	HEAVY_RUNTIME_RATIONALE "Exercises editor texture import, build, cache, and render-resource contracts."
	DATA_DIRECTORIES "${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport" "${CMAKE_CURRENT_SOURCE_DIR}/Data"
)

durin_add_native_test(TextureCompressionQualificationTests
	KIND qualification
	DOMAINS texture
	MODULES texture-compressor
	STACKS editor
	SOURCES Private/Texture/TextureCompressionQualificationTests.cpp
	INCLUDE_DIRECTORIES ${_durin_texture_test_include_directories}
	LIBRARIES Core CoreDObject Engine TextureCompressor
	REQUIRES editor
	REQUIREMENT_RATIONALE "Measures the editor-only CPU texture compression provider."
	TIMEOUT 600
)

durin_add_native_test(DerivedDataTextureQualificationTests
	KIND qualification
	DOMAINS texture derived-data
	MODULES engine texture-compressor
	SOURCES
		Private/Texture/DerivedDataTextureQualificationTests.cpp
	PRIVATE_SOURCES
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/Engine/Private/Texture/TextureCubeSourceBuild.cpp
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/Engine/Private/Texture/TextureBuildSession.cpp
	PRIVATE_SOURCE_OWNER Engine
	PRIVATE_SOURCE_RATIONALE "Measures the Engine-owned captured Cube source boundary and its private session dependency without exporting test-only DLL symbols."
	INCLUDE_DIRECTORIES ${_durin_texture_test_include_directories}
		${DURIN_PROJECT_SOURCE_DIR}/Runtime/Engine/Private
	LIBRARIES Core CoreDObject Engine TextureCompressor MeshBuilder DerivedDataCache
	REQUIRES editor
	REQUIREMENT_RATIONALE "Measures cold and warm authored texture build boundaries using isolated caches."
	TIMEOUT 600
)

durin_add_native_test(SceneImportTests
	REQUIRES editor
	REQUIREMENT_RATIONALE
		"Texture processing and scene import require TextureCompressor and AssetForgeBuiltins editor services."
	KIND integration
	DOMAINS asset-import
	MODULES engine texture-compressor asset-forge-builtins content-browser
	STACKS editor
	TIMEOUT 600
	SOURCES Private/Texture/SceneImportTests.cpp
	INCLUDE_DIRECTORIES ${_durin_texture_test_include_directories}
	LIBRARIES ShaderBuild ${_durin_texture_test_libraries} TextureCompressor ContentBrowser bc7enc_rdo::bc7enc_rdo
	HEAVY_RUNTIME_RATIONALE
		"Exercises editor scene-import publication and rollback across runtime asset families."
	DATA_DIRECTORIES "${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport"
)
