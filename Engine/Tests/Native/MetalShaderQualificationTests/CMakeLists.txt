if(NOT APPLE OR NOT DURIN_WITH_EDITOR)
	durin_exclude_native_test_sources(
		RATIONALE "Direct Metal shader qualification requires macOS and the editor shader toolchain."
		SOURCES Private/MetalShaderQualificationTests.mm Private/MetalMaterialQualificationTests.mm
	)
	return()
endif()

enable_language(OBJCXX)
set_source_files_properties(
	Private/MetalShaderQualificationTests.mm
	Private/MetalMaterialQualificationTests.mm
	PROPERTIES
	SKIP_PRECOMPILE_HEADERS ON
	COMPILE_FLAGS "-fobjc-arc"
)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)

durin_add_native_test(MetalShaderQualificationTests
	KIND qualification
	DOMAINS renderer shader material
	MODULES engine shaderbuild
	BACKENDS metal
	SOURCES Private/MetalShaderQualificationTests.mm Private/MetalMaterialQualificationTests.mm
	LIBRARIES Core CoreDObject Engine RenderCore ShaderBuild Slang_Imported
		${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} spirv-cross-c
	ENVIRONMENTS authored-shaders
	REQUIRES editor
	REQUIREMENT_RATIONALE "Production material shader compilation uses the authored editor shader environment."
	DATA_FILES
		"${DURIN_PROJECT_ROOT_DIR}/Shaders/Slang/GBufferGPUCulling.slang"
		"${DURIN_PROJECT_ROOT_DIR}/Shaders/Slang/ImGui.slang"
	DATA_DIRECTORIES "${DURIN_PROJECT_ROOT_DIR}/Tests/Data/MetalShaderQualification"
	RESOURCE_LOCKS "durin-gpu"
)
