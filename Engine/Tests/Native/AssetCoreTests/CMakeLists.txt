include(GoogleTest)

add_durin_test(AssetCoreTests
	Private/AssetImportTests.cpp
	Private/ImageDecoderTests.cpp
	Private/PackageTests.cpp
)

target_link_libraries(AssetCoreTests PRIVATE
	Core
	AssetCore
	GTest::gtest_main
)

durin_test_deploy_target_binary(AssetCoreTests Core)
durin_test_deploy_target_binary(AssetCoreTests CoreDObject)
durin_test_deploy_target_binary(AssetCoreTests AssetCore)
durin_test_deploy_runtime_files(AssetCoreTests assimp::assimp)
durin_test_deploy_directory_to_data(AssetCoreTests "${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport")

get_target_property(_durin_asset_core_tests_work_dir AssetCoreTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(AssetCoreTests
	WORKING_DIRECTORY "${_durin_asset_core_tests_work_dir}"
)
