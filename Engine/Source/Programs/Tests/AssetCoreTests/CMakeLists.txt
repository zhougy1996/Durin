include(GoogleTest)

add_durin_test(AssetCoreTests
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

durin_target_copy_binary_to_output_dir(AssetCoreTests Core)
durin_target_copy_binary_to_output_dir(AssetCoreTests AssetCore)
durin_target_copy_binary_to_output_dir(AssetCoreTests assimp::assimp)
durin_target_deploy_runtime_files(AssetCoreTests assimp::assimp)

gtest_discover_tests(AssetCoreTests
	WORKING_DIRECTORY $<TARGET_FILE_DIR:AssetCoreTests>
)
