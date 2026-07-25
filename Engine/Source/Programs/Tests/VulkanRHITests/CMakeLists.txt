include(GoogleTest)

add_durin_test(VulkanRHITests
	Private/VulkanTextureSamplingTests.cpp
	../../../Runtime/RenderCore/Private/Shader/SlangShaderCompiler.cpp
	../../../Runtime/RenderCore/Private/Shader/ShaderCompileUtilities.cpp
)

target_include_directories(VulkanRHITests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/RenderCore/Private
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/VulkanRHI/Public
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/VulkanRHI/Private
)

target_link_libraries(VulkanRHITests PRIVATE
	Core
	CoreDObject
	AssetCore
	Engine
	RHI
	RenderCore
	Vulkan::Vulkan
	Slang_Imported
	GTest::gtest_main
)

durin_test_deploy_target_binary(VulkanRHITests Core)
durin_test_deploy_target_binary(VulkanRHITests CoreDObject)
durin_test_deploy_target_binary(VulkanRHITests AssetCore)
durin_test_deploy_target_binary(VulkanRHITests Engine)
durin_test_deploy_target_binary(VulkanRHITests RHI)
durin_test_deploy_target_binary(VulkanRHITests RenderCore)
durin_test_deploy_target_binary(VulkanRHITests VulkanRHI)
durin_test_deploy_runtime_files(VulkanRHITests Slang_Imported)
durin_test_deploy_directory_to_data(VulkanRHITests "${CMAKE_CURRENT_SOURCE_DIR}/TestData")

get_target_property(_durin_vulkan_rhi_tests_work_dir VulkanRHITests DURIN_TEST_WORK_DIR)

gtest_discover_tests(VulkanRHITests
	WORKING_DIRECTORY "${_durin_vulkan_rhi_tests_work_dir}"
)
