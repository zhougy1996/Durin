include(GoogleTest)

function(durin_add_asset_functional_test target_name)
	set(options IMPORT_RUNTIME)
	set(one_value_args TIMEOUT)
	set(multi_value_args SOURCES LIBRARIES)
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
		${DURIN_ASSET_TEST_LIBRARIES}
	)

	if(DURIN_ASSET_TEST_IMPORT_RUNTIME)
		target_link_libraries(${target_name} PRIVATE AssetImportCore StandardAssetImport)
		durin_test_deploy_directory_to_data(
			${target_name}
			"${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport"
		)
	endif()

	set_target_properties(${target_name} PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
	)
	if(DURIN_ASSET_TEST_TIMEOUT)
		set_target_properties(${target_name} PROPERTIES
			DURIN_TEST_TIMEOUT "${DURIN_ASSET_TEST_TIMEOUT}"
		)
	endif()
	durin_discover_tests(${target_name})
endfunction()

durin_add_asset_functional_test(AssetPackageTests
	SOURCES Private/PackageTests.cpp
)
durin_test_deploy_directory_to_data(
	AssetPackageTests
	"${CMAKE_CURRENT_SOURCE_DIR}/Data/Compatibility"
)

durin_add_asset_functional_test(AssetCookTests
	TIMEOUT 600
	SOURCES Private/CookedAssetTests.cpp
)

durin_add_asset_functional_test(AssetDerivedDataTests
	SOURCES Private/DerivedDataObjectStoreTests.cpp
)

durin_add_asset_functional_test(AssetDecodeTests
	SOURCES Private/ImageDecoderTests.cpp
)

if(DURIN_WITH_EDITOR)
	durin_add_asset_functional_test(AssetImportTests
		IMPORT_RUNTIME
		TIMEOUT 600
		SOURCES Private/AssetImportTests.cpp
	)

	durin_add_asset_functional_test(AssetImportCoreTests
		LIBRARIES AssetImportCore
		SOURCES Private/AssetImportCoreTests.cpp Private/ImportRecordTests.cpp
	)
else()
	durin_exclude_native_test_sources(
		RATIONALE "Asset import services and their test fixtures are editor-only."
		SOURCES
			Private/AssetImportTests.cpp
			Private/AssetImportCoreTests.cpp
			Private/ImportRecordTests.cpp
	)
endif()
