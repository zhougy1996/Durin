if(APPLE AND NOT DURIN_ENABLE_APPLICATION_TESTS)
	durin_exclude_native_test_sources(
		RATIONALE
			"macOS Vulkan qualification requires explicitly enabled LaunchServices application tests."
		SOURCES Private/VulkanRHITestEnvironment.cpp
			Private/VulkanCreationQualificationTests.cpp
			Private/VulkanFailureInjectionTests.cpp
			Private/VulkanMemoryPolicyTests.cpp
			Private/VulkanTextureSamplingTests.cpp
			Private/VulkanResourceTransitionTests.cpp
			Private/VulkanSwapchainSelectionTests.cpp
	)
	return()
endif()

if(NOT DURIN_WITH_EDITOR)
	durin_exclude_native_test_sources(
		RATIONALE
			"This integration fixture compiles authored Slang test shaders and is therefore excluded from the compiler-free Game closure."
		SOURCES Private/VulkanRHITestEnvironment.cpp
			Private/VulkanCreationQualificationTests.cpp
			Private/VulkanFailureInjectionTests.cpp
			Private/VulkanMemoryPolicyTests.cpp
			Private/VulkanTextureSamplingTests.cpp
			Private/VulkanResourceTransitionTests.cpp
			Private/VulkanSwapchainSelectionTests.cpp
	)
	return()
endif()

set(_durin_vulkan_rhi_test_kind integration)
if(APPLE)
	# MoltenVK hardware execution is part of the surface-qualified M3 gate. Keep
	# the target buildable on macOS, but out of the ordinary non-GPU aggregate.
	set(_durin_vulkan_rhi_test_kind qualification)
	set(_durin_vulkan_rhi_execution_host EXECUTION_HOST application)
endif()
set(_durin_vulkan_platform_libraries)
if(WIN32)
	set(_durin_vulkan_platform_libraries dxgi)
endif()

durin_add_native_test(VulkanRHIIntegrationTests
	KIND ${_durin_vulkan_rhi_test_kind}
	${_durin_vulkan_rhi_execution_host}
	DOMAINS renderer viewport
	MODULES rendercore shaderbuild rhi vulkan-rhi
	BACKENDS vulkan
	STACKS renderer
	PROCESSORS 8
	TIMEOUT 900
	PRIVATE_SOURCE_OWNER ShaderBuild
	PRIVATE_SOURCE_RATIONALE "Owns the ShaderBuild compiler white-box seam used by Vulkan integration coverage."
	SOURCES
		Private/VulkanRHITestEnvironment.cpp
		Private/VulkanFailureInjectionTests.cpp
		Private/VulkanMemoryPolicyTests.cpp
		Private/VulkanTextureSamplingTests.cpp
		Private/VulkanResourceTransitionTests.cpp
		Private/VulkanSwapchainSelectionTests.cpp
		${DURIN_PROJECT_SOURCE_DIR}/Developer/ShaderBuild/Private/SlangSessionEnvironment.cpp
		${DURIN_PROJECT_SOURCE_DIR}/Developer/ShaderBuild/Private/SlangShaderCompiler.cpp
		${DURIN_PROJECT_SOURCE_DIR}/Developer/ShaderBuild/Private/ShaderCompileUtilities.cpp
	COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
	INCLUDE_DIRECTORIES
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/RenderCore/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Developer/ShaderBuild/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Developer/TextureBuild/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/VulkanRHI/Public
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/VulkanRHI/Private
	LIBRARIES
		Core
		CoreDObject
		Engine
		RHI
		RenderCore
		ShaderBuild
		VulkanRHI
		Vulkan::Vulkan
		Slang_Imported
		TextureBuild
	RESOURCE_LOCKS "durin-gpu;durin-rhi-lifecycle"
	HEAVY_RUNTIME_RATIONALE "Failure-injection tests must share the test-only state compiled into VulkanRHI."
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
)

durin_add_native_test(VulkanCreationQualificationTests
	KIND qualification
	DOMAINS renderer rhi-creation
	MODULES rhi vulkan-rhi shaderbuild
	BACKENDS vulkan
	PROCESSORS 8
	TIMEOUT 900
	PRIVATE_SOURCE_OWNER ShaderBuild
	PRIVATE_SOURCE_RATIONALE "Compiles fixed qualification shader inputs before timed device lifetimes."
	SOURCES
		Private/VulkanCreationQualificationTests.cpp
		${DURIN_PROJECT_SOURCE_DIR}/Developer/ShaderBuild/Private/SlangSessionEnvironment.cpp
		${DURIN_PROJECT_SOURCE_DIR}/Developer/ShaderBuild/Private/SlangShaderCompiler.cpp
		${DURIN_PROJECT_SOURCE_DIR}/Developer/ShaderBuild/Private/ShaderCompileUtilities.cpp
	COMPILE_DEFINITIONS DURIN_VULKAN_TEST_FAILURE_INJECTION=1
	INCLUDE_DIRECTORIES
		${CMAKE_SOURCE_DIR}/Engine/Source/Developer/ShaderBuild/Private
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/VulkanRHI/Private
	LIBRARIES
		Core
		RHI
		RenderCore
		ShaderBuild
		VulkanRHI
		Vulkan::Vulkan
		Slang_Imported
		${_durin_vulkan_platform_libraries}
	RESOURCE_LOCKS "durin-gpu;durin-rhi-lifecycle"
	HEAVY_RUNTIME_RATIONALE "Measures real Vulkan creation with the opt-in backend timing seam."
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
)
