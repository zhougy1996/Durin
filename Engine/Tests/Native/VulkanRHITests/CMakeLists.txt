include(GoogleTest)

add_durin_test(VulkanRHIIntegrationTests
	Private/VulkanTextureSamplingTests.cpp
	${DURIN_PROJECT_SOURCE_DIR}/Runtime/RenderCore/Private/Shader/SlangShaderCompiler.cpp
	${DURIN_PROJECT_SOURCE_DIR}/Runtime/RenderCore/Private/Shader/ShaderCompileUtilities.cpp
)

target_include_directories(VulkanRHIIntegrationTests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/RenderCore/Private
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/VulkanRHI/Public
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/VulkanRHI/Private
)

target_link_libraries(VulkanRHIIntegrationTests PRIVATE
	Core
	CoreDObject
	AssetCore
	Engine
	RHI
	RenderCore
	Vulkan::Vulkan
	Slang_Imported
)

durin_test_register_runtime_only_dependencies(VulkanRHIIntegrationTests
	RATIONALE "RHIInit selects the Vulkan backend dynamically at runtime."
	TARGETS VulkanRHI
)
durin_test_deploy_directory_to_data(VulkanRHIIntegrationTests "${CMAKE_CURRENT_SOURCE_DIR}/Data")

set_target_properties(VulkanRHIIntegrationTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
	DURIN_TEST_RESOURCE_LOCKS durin-gpu
	DURIN_TEST_TIMEOUT 900
)

durin_discover_tests(VulkanRHIIntegrationTests)
