if(DURIN_WITH_EDITOR)
	add_durin_test(TextureTests
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
		MODULES asset-tools engine texture-build static-mesh-build skeletal-build asset-forge-builtins texture-editor
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
		TextureBuild
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
		MODULES engine texture-build asset-forge-builtins
		STACKS editor
		TIMEOUT 600
	)
else()
	durin_exclude_native_test_sources(
		RATIONALE "Texture source processing and scene import require the editor TextureBuild and AssetForgeBuiltins modules."
		SOURCES
			Private/Texture/TextureTestEnvironment.cpp
			Private/Texture/TextureImportAndCacheTests.cpp
			Private/Texture/TextureDerivedDataTests.cpp
			Private/Texture/TextureBuildTests.cpp
			Private/Texture/VolumeTextureSourceImportTests.cpp
			Private/Texture/TextureFailureTests.cpp
			Private/Texture/SceneImportTests.cpp
			Private/Texture/SingleAssetImportTests.cpp
			Private/Texture/EquirectangularTextureCubeTests.cpp
			Private/TextureCubeTests.cpp
	)
endif()
