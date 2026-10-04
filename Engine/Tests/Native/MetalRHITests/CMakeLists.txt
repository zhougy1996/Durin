if(NOT APPLE)
	durin_exclude_native_test_sources(
		RATIONALE "Native Metal device admission requires macOS."
		SOURCES Private/MetalRHIHeadlessTests.cpp Private/MetalRHIResourceTests.mm)
	return()
endif()

enable_language(OBJCXX)
set_source_files_properties(Private/MetalRHIResourceTests.mm PROPERTIES
	SKIP_PRECOMPILE_HEADERS ON COMPILE_FLAGS "-fobjc-arc")
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
set(_metal_rhi_test_libraries Core RHI MetalRHI
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
	RESOURCE_LOCKS "durin-gpu;durin-rhi-lifecycle"
)
