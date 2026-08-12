include(GoogleTest)

add_durin_test(VulkanRHIIntegrationTests
	Private/VulkanFailureInjectionTests.cpp
	Private/VulkanMemoryPolicyTests.cpp
	Private/VulkanTextureSamplingTests.cpp
	Private/VulkanResourceTransitionTests.cpp
	Private/VulkanSwapchainSelectionTests.cpp
	${DURIN_PROJECT_SOURCE_DIR}/Runtime/RenderCore/Private/Shader/SlangShaderCompiler.cpp
	${DURIN_PROJECT_SOURCE_DIR}/Runtime/RenderCore/Private/Shader/ShaderCompileUtilities.cpp
)

target_compile_definitions(VulkanRHIIntegrationTests PRIVATE
	DURIN_VULKAN_TEST_FAILURE_INJECTION=1
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
	EngineAssetBuild
	RHI
	RenderCore
	VulkanRHI
	Vulkan::Vulkan
	Slang_Imported
)
durin_test_deploy_directory_to_data(VulkanRHIIntegrationTests "${CMAKE_CURRENT_SOURCE_DIR}/Data")

set_target_properties(VulkanRHIIntegrationTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
	DURIN_TEST_RESOURCE_LOCKS durin-gpu
	DURIN_TEST_TIMEOUT 900
	DURIN_TEST_HEAVY_RUNTIME_RATIONALE
		"Failure-injection tests must share the test-only state compiled into VulkanRHI."
)
durin_finalize_native_test(VulkanRHIIntegrationTests
	KIND integration
	DOMAINS renderer viewport
	MODULES rendercore rhi vulkan-rhi
	BACKENDS vulkan
	STACKS renderer
	PRIVATE_SOURCE_OWNER RenderCore
	PRIVATE_SOURCE_RATIONALE
		"Owns the RenderCore shader-compiler white-box seam used by Vulkan integration coverage."
)

durin_discover_tests(VulkanRHIIntegrationTests)
