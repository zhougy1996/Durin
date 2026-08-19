include(GoogleTest)

if(APPLE AND NOT DURIN_ENABLE_APPLICATION_TESTS)
	durin_exclude_native_test_sources(
		RATIONALE
			"macOS Vulkan qualification requires explicitly enabled LaunchServices application tests."
		SOURCES
			Private/VulkanRHITestEnvironment.cpp
			Private/VulkanFailureInjectionTests.cpp
			Private/VulkanMemoryPolicyTests.cpp
			Private/VulkanTextureSamplingTests.cpp
			Private/VulkanResourceTransitionTests.cpp
			Private/VulkanSwapchainSelectionTests.cpp
	)
	return()
endif()

add_durin_test(VulkanRHIIntegrationTests
	Private/VulkanRHITestEnvironment.cpp
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
	RHI
	RenderCore
	VulkanRHI
	Vulkan::Vulkan
	Slang_Imported
)
if(DURIN_WITH_EDITOR)
	target_link_libraries(VulkanRHIIntegrationTests PRIVATE TextureBuild)
endif()
set_target_properties(VulkanRHIIntegrationTests PROPERTIES
	DURIN_TEST_RESOURCE_LOCKS durin-gpu
	DURIN_TEST_HEAVY_RUNTIME_RATIONALE
		"Failure-injection tests must share the test-only state compiled into VulkanRHI."
)
set(_durin_vulkan_rhi_test_kind integration)
if(APPLE)
	# MoltenVK hardware execution is part of the surface-qualified M3 gate. Keep
	# the target buildable on macOS, but out of the ordinary non-GPU aggregate.
	set(_durin_vulkan_rhi_test_kind qualification)
	set(_durin_vulkan_rhi_execution_host EXECUTION_HOST application)
endif()
durin_test_deploy_directory_to_data(VulkanRHIIntegrationTests "${CMAKE_CURRENT_SOURCE_DIR}/Data")

durin_register_native_test(VulkanRHIIntegrationTests
	KIND ${_durin_vulkan_rhi_test_kind}
	${_durin_vulkan_rhi_execution_host}
	DOMAINS renderer viewport
	MODULES rendercore rhi vulkan-rhi
	BACKENDS vulkan
	STACKS renderer
	TIMEOUT 900
	PRIVATE_SOURCE_OWNER RenderCore
	PRIVATE_SOURCE_RATIONALE
		"Owns the RenderCore shader-compiler white-box seam used by Vulkan integration coverage."
)
