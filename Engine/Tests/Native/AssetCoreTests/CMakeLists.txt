include(GoogleTest)

add_durin_test(AssetCoreTests
	Private/AssetImportTests.cpp
	Private/CookedAssetTests.cpp
	Private/DerivedDataObjectStoreTests.cpp
	Private/ImageDecoderTests.cpp
	Private/PackageTests.cpp
)

target_link_libraries(AssetCoreTests PRIVATE
	Core
	AssetCore
	AssetImport
	GTest::gtest_main
)

durin_test_deploy_target_binary(AssetCoreTests Core)
durin_test_deploy_target_binary(AssetCoreTests CoreDObject)
durin_test_deploy_target_binary(AssetCoreTests AssetCore)
durin_test_deploy_target_binary(AssetCoreTests AssetImport)
durin_test_deploy_runtime_files(AssetCoreTests assimp::assimp)
durin_test_deploy_directory_to_data(AssetCoreTests "${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport")

durin_discover_tests(AssetCoreTests)
