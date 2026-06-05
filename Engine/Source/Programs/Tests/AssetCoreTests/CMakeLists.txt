include(GoogleTest)

add_durin_test(AssetCoreTests
	Private/AssetImportTests.cpp
)

target_link_libraries(AssetCoreTests PRIVATE
	Core
	AssetCore
	GTest::gtest_main
)

durin_test_deploy_target_binary(AssetCoreTests Core)
durin_test_deploy_target_binary(AssetCoreTests AssetCore)
durin_test_deploy_runtime_files(AssetCoreTests assimp::assimp)
durin_test_deploy_directory_to_data(AssetCoreTests "${CMAKE_CURRENT_SOURCE_DIR}/TestData")

get_target_property(_durin_asset_core_tests_work_dir AssetCoreTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(AssetCoreTests
	WORKING_DIRECTORY "${_durin_asset_core_tests_work_dir}"
)
