if(NOT APPLE)
	durin_exclude_native_test_sources(
		RATIONALE "Native Metal device admission requires macOS."
		SOURCES Private/MetalRHIHeadlessTests.cpp Private/MetalRHIResourceTests.mm Private/MetalSkyLightingTests.cpp
			Private/MetalSceneQualificationTests.cpp Private/MetalCookedScenePreparationTests.cpp)
	return()
endif()

enable_language(OBJCXX)
set_source_files_properties(Private/MetalRHIHeadlessTests.cpp PROPERTIES COMPILE_FLAGS "-fblocks")
set_source_files_properties(Private/MetalRHIResourceTests.mm PROPERTIES
	SKIP_PRECOMPILE_HEADERS ON COMPILE_FLAGS "-fobjc-arc")
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
set(_metal_rhi_test_libraries Core RHI MetalRHI ShaderFormat
	${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
set(_metal_rhi_test_domains rhi-creation)
set(_metal_rhi_test_backends metal)
if(DURIN_WITH_EDITOR)
	list(APPEND _metal_rhi_test_libraries RenderCore ShaderBuild)
	list(APPEND _metal_rhi_test_domains renderer shader)
	list(APPEND _metal_rhi_test_backends vulkan)
endif()

durin_add_native_test(MetalRHIHeadlessTests
	KIND qualification
	DOMAINS ${_metal_rhi_test_domains}
	MODULES rhi
	BACKENDS ${_metal_rhi_test_backends}
	SOURCES Private/MetalRHIHeadlessTests.cpp Private/MetalRHIResourceTests.mm
	LIBRARIES ${_metal_rhi_test_libraries}
	INCLUDE_DIRECTORIES
		"${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/MetalRHI/Private"
		"${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/MetalRHI/Private/MacOS"
		"${CMAKE_SOURCE_DIR}/Engine/External/Source/metal-cpp"
	RESOURCE_LOCKS "durin-gpu;durin-rhi-lifecycle"
)

# Scene-owned environment filtering exercises the complete renderer lifecycle.
durin_add_native_test(MetalSceneQualificationTests
    KIND qualification
    DOMAINS renderer
    MODULES renderer engine
    BACKENDS metal vulkan
    STACKS renderer
    SOURCES Private/MetalSceneQualificationTests.cpp
    LIBRARIES Core CoreDObject Engine ApplicationCore RenderCore Renderer MetalRHI VulkanRHI
    INCLUDE_DIRECTORIES
        "${CMAKE_SOURCE_DIR}/Engine/Tests/Native/EngineTests/Private"
        "${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private"
    REQUIRES editor
    REQUIREMENT_RATIONALE "Compiles authored scene shaders for production RDG route qualification."
    ENVIRONMENTS authored-shaders
    RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
    HEAVY_RUNTIME_RATIONALE "Exercises headless native Metal graph stages, attachment handoffs, readback and failure propagation."
    TIMEOUT 600
)

durin_add_native_test(MetalSkyLightingTests
    KIND qualification
    DOMAINS renderer sky-box
    MODULES renderer engine texture-compressor
    BACKENDS metal vulkan
    STACKS editor renderer
    SOURCES Private/MetalSkyLightingTests.cpp
    LIBRARIES Core CoreDObject Engine RenderCore Renderer TextureCompressor AssetTools MetalRHI VulkanRHI
    INCLUDE_DIRECTORIES
        "${CMAKE_SOURCE_DIR}/Engine/Tests/Native/EngineTests/Private"
        "${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private"
    REQUIRES editor
    REQUIREMENT_RATIONALE "Builds authored HDR cubes and production sky-light shaders."
    ENVIRONMENTS authored-shaders
    RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
    HEAVY_RUNTIME_RATIONALE "Exercises headless Metal scene-owned environment filtering and GPU readback."
    TIMEOUT 600
)

# Cook once, retire the producer, then exercise two real device lifetimes.
durin_add_native_test(MetalCookedScenePreparationTests
    KIND qualification
    DOMAINS renderer shader asset-cook
    MODULES renderer engine rendercore shaderbuild
    BACKENDS metal
    STACKS renderer
    SOURCES Private/MetalCookedScenePreparationTests.cpp
    LIBRARIES Core CoreDObject Engine ApplicationCore RenderCore Renderer MetalRHI
    INCLUDE_DIRECTORIES
        "${CMAKE_SOURCE_DIR}/Engine/Tests/Native/EngineTests/Private"
        "${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Renderer/Private"
    REQUIRES editor
    REQUIREMENT_RATIONALE "Builds binary material/fixed products before retiring ShaderBuild for cooked runtime qualification."
    ENVIRONMENTS authored-shaders
    RESOURCE_LOCKS durin-gpu durin-rhi-lifecycle
    HEAVY_RUNTIME_RATIONALE "Exercises compiler-free native Metal scene preparation, first draw, scene release and device recreation."
    TIMEOUT 300
)
