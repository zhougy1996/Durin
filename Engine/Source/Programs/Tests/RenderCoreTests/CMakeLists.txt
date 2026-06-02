include(GoogleTest)

durin_add_test_target(RenderCoreTests
	Private/ShaderFoundationTests.cpp
)

target_link_libraries(RenderCoreTests PRIVATE
	Core
	RHI
	RenderCore
	GTest::gtest_main
)

durin_copy_target_binary_to_output_dir(RenderCoreTests Core)
durin_copy_target_binary_to_output_dir(RenderCoreTests RHI)
durin_copy_target_binary_to_output_dir(RenderCoreTests RenderCore)
durin_deploy_runtime_files_for_target(RenderCoreTests Slang_Imported)

gtest_discover_tests(RenderCoreTests
	WORKING_DIRECTORY $<TARGET_FILE_DIR:RenderCoreTests>
)
