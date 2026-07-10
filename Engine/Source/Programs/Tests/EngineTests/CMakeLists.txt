include(GoogleTest)

add_durin_test(EngineTests
	Private/WorldTests.cpp
)

target_link_libraries(EngineTests PRIVATE
	Core
	CoreDObject
	AssetCore
	Engine
	GTest::gtest_main
)

durin_test_deploy_target_binary(EngineTests Core)
durin_test_deploy_target_binary(EngineTests CoreDObject)
durin_test_deploy_target_binary(EngineTests Engine)
durin_test_deploy_target_binary(EngineTests ApplicationCore)
durin_test_deploy_target_binary(EngineTests AssetCore)
durin_test_deploy_target_binary(EngineTests MonaCore)
durin_test_deploy_target_binary(EngineTests Mona)
durin_test_deploy_target_binary(EngineTests RHI)
durin_test_deploy_target_binary(EngineTests RenderCore)
durin_test_deploy_runtime_files(EngineTests assimp::assimp)
durin_test_deploy_runtime_files(EngineTests Slang_Imported)

get_target_property(_durin_engine_tests_work_dir EngineTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(EngineTests
	WORKING_DIRECTORY "${_durin_engine_tests_work_dir}"
)
