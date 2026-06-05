include(GoogleTest)

add_durin_test(RenderCoreTests
	Private/ShaderFoundationTests.cpp
)

target_link_libraries(RenderCoreTests PRIVATE
	Core
	RHI
	RenderCore
	GTest::gtest_main
)

durin_test_deploy_target_binary(RenderCoreTests Core)
durin_test_deploy_target_binary(RenderCoreTests RHI)
durin_test_deploy_target_binary(RenderCoreTests RenderCore)
durin_test_deploy_runtime_files(RenderCoreTests Slang_Imported)

get_target_property(_durin_render_core_tests_work_dir RenderCoreTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(RenderCoreTests
	WORKING_DIRECTORY "${_durin_render_core_tests_work_dir}"
)
