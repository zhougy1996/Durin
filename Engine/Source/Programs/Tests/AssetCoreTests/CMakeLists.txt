include(GoogleTest)

durin_add_test_target(AssetCoreTests
	Private/AssetImportTests.cpp
)

target_compile_definitions(AssetCoreTests PRIVATE
	ASSETCORE_TEST_SAMPLE_OBJ_FILE="${CMAKE_CURRENT_SOURCE_DIR}/TestData/Triangle.obj"
)

target_link_libraries(AssetCoreTests PRIVATE
	Core
	AssetCore
	GTest::gtest_main
)

durin_copy_target_binary_to_output_dir(AssetCoreTests Core)
durin_copy_target_binary_to_output_dir(AssetCoreTests AssetCore)
durin_copy_target_binary_to_output_dir(AssetCoreTests assimp::assimp)
durin_deploy_runtime_files_for_target(AssetCoreTests assimp::assimp)

gtest_discover_tests(AssetCoreTests
	WORKING_DIRECTORY $<TARGET_FILE_DIR:AssetCoreTests>
)
