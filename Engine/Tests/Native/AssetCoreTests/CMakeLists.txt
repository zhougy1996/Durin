include(GoogleTest)

function(durin_add_asset_functional_test target_name)
	set(options IMPORT_RUNTIME)
	set(one_value_args)
	set(multi_value_args SOURCES)
	cmake_parse_arguments(
		DURIN_ASSET_TEST
		"${options}"
		"${one_value_args}"
		"${multi_value_args}"
		${ARGN}
	)

	add_durin_test(${target_name} ${DURIN_ASSET_TEST_SOURCES})
	target_link_libraries(${target_name} PRIVATE
		Core
		CoreDObject
		AssetCore
		GTest::gtest_main
	)
	durin_test_deploy_target_binary(${target_name} Core)
	durin_test_deploy_target_binary(${target_name} CoreDObject)
	durin_test_deploy_target_binary(${target_name} AssetCore)

	if(DURIN_ASSET_TEST_IMPORT_RUNTIME)
		target_link_libraries(${target_name} PRIVATE AssetImport)
		durin_test_deploy_target_binary(${target_name} AssetImport)
		durin_test_deploy_runtime_files(${target_name} assimp::assimp)
		durin_test_deploy_directory_to_data(
			${target_name}
			"${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport"
		)
	endif()

	durin_discover_tests(${target_name})
endfunction()

durin_add_asset_functional_test(AssetPackageTests
	SOURCES Private/PackageTests.cpp
)

durin_add_asset_functional_test(AssetCookTests
	SOURCES Private/CookedAssetTests.cpp
)
set_target_properties(AssetCookTests PROPERTIES DURIN_TEST_TIMEOUT 600)

durin_add_asset_functional_test(AssetDerivedDataTests
	SOURCES Private/DerivedDataObjectStoreTests.cpp
)

durin_add_asset_functional_test(AssetDecodeTests
	SOURCES Private/ImageDecoderTests.cpp
)

durin_add_asset_functional_test(AssetImportTests
	IMPORT_RUNTIME
	SOURCES Private/AssetImportTests.cpp
)
set_target_properties(AssetImportTests PROPERTIES DURIN_TEST_TIMEOUT 600)
