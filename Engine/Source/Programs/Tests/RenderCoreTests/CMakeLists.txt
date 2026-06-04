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

durin_target_copy_binary_to_output_dir(RenderCoreTests Core)
durin_target_copy_binary_to_output_dir(RenderCoreTests RHI)
durin_target_copy_binary_to_output_dir(RenderCoreTests RenderCore)
durin_target_deploy_runtime_files(RenderCoreTests Slang_Imported)

gtest_discover_tests(RenderCoreTests
	WORKING_DIRECTORY $<TARGET_FILE_DIR:RenderCoreTests>
)
