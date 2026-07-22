include(GoogleTest)

add_durin_test(RenderCoreTests
	Private/ShaderFoundationTests.cpp
	Private/ShaderCacheStoreTests.cpp
	Private/ShaderCompileServiceTests.cpp
	Private/ShaderReflectionTests.cpp
	Private/RenderTargetLayoutTests.cpp
	../../../Runtime/RenderCore/Private/Shader/SlangShaderCompiler.cpp
	../../../Runtime/RenderCore/Private/Shader/ShaderCompileUtilities.cpp
	../../../Runtime/RenderCore/Private/Shader/ShaderCacheStore.cpp
	../../../Runtime/RenderCore/Private/Shader/ShaderCompileService.cpp
	../../../Runtime/RenderCore/Private/Shader/SlangShaderDependencyResolver.cpp
)

target_include_directories(RenderCoreTests PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/../../../Runtime/RenderCore/Private"
)

target_link_libraries(RenderCoreTests PRIVATE
	Core
	RHI
	RenderCore
	Slang_Imported
	GTest::gtest_main
)

durin_test_deploy_target_binary(RenderCoreTests Core)
durin_test_deploy_target_binary(RenderCoreTests RHI)
durin_test_deploy_target_binary(RenderCoreTests RenderCore)
durin_test_deploy_runtime_files(RenderCoreTests Slang_Imported)
durin_test_deploy_directory_to_data(RenderCoreTests "${CMAKE_CURRENT_SOURCE_DIR}/TestData")

get_target_property(_durin_render_core_tests_work_dir RenderCoreTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(RenderCoreTests
	WORKING_DIRECTORY "${_durin_render_core_tests_work_dir}"
)
