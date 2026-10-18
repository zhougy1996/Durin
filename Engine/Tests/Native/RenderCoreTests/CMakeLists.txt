set(_durin_render_core_private_dir
	"${DURIN_PROJECT_SOURCE_DIR}/Runtime/RenderCore/Private")
set(_durin_shader_build_private_dir
	"${DURIN_PROJECT_SOURCE_DIR}/Developer/ShaderBuild/Private")
set(_durin_shader_compiler_definitions)
set(_durin_shader_compiler_options)
if(APPLE AND DURIN_WITH_EDITOR)
	# White-box compiler sources must use the producer's backend revision too.
	list(APPEND _durin_shader_compiler_definitions
		DURIN_SHADER_SPIRV_CROSS_REVISION="$<TARGET_PROPERTY:ShaderBuild,DURIN_SHADER_SPIRV_CROSS_REVISION>")
	set(_durin_shader_compiler_options COMPILE_DEFINITIONS ${_durin_shader_compiler_definitions})
endif()

durin_add_native_test(RenderShaderCookedLibraryTests
	KIND contract
	DOMAINS shader
	MODULES rendercore
	SOURCES Private/ShaderCookedLibraryTests.cpp
	LIBRARIES Core RHI RenderCore
	INCLUDE_DIRECTORIES "${_durin_render_core_private_dir}" "${_durin_shader_build_private_dir}"
)

if(DURIN_WITH_EDITOR)
durin_add_native_test(RenderShaderContractTests
	KIND contract
	DOMAINS shader
	MODULES rendercore shaderbuild
	PRIVATE_SOURCE_RATIONALE
		"RenderCore-owned shader compiler white-box coverage avoids exporting private compiler implementation symbols."
	SOURCES Private/ShaderFoundationTests.cpp Private/ShaderReflectionTests.cpp
	PRIVATE_SOURCES
		${_durin_shader_build_private_dir}/SlangSessionEnvironment.cpp
		${_durin_shader_build_private_dir}/SlangShaderCompiler.cpp
		${_durin_shader_build_private_dir}/ShaderCompileUtilities.cpp
		${_durin_shader_build_private_dir}/SlangShaderDependencyResolver.cpp
	LIBRARIES Core RHI RenderCore DerivedDataCache ShaderBuild Slang_Imported
	INCLUDE_DIRECTORIES "${_durin_render_core_private_dir}" "${_durin_shader_build_private_dir}"
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
	TIMEOUT 600
	PRIVATE_SOURCE_OWNER ShaderBuild
	COMPILE_DEFINITIONS ${_durin_shader_compiler_definitions}
		DURIN_ENGINE_SHADER_SOURCE_DIR="${DURIN_PROJECT_ROOT_DIR}/Shaders/Slang"
)

durin_add_native_test(RenderShaderCacheTests
	${_durin_shader_compiler_options}
	KIND feature
	DOMAINS shader
	MODULES rendercore shaderbuild derived-data-cache
	PRIVATE_SOURCE_RATIONALE
		"RenderCore-owned shader cache white-box coverage avoids exporting private cache implementation symbols."
	SOURCES Private/ShaderDerivedDataTests.cpp
	PRIVATE_SOURCES
		${_durin_shader_build_private_dir}/SlangSessionEnvironment.cpp
		${_durin_shader_build_private_dir}/SlangShaderCompiler.cpp
		${_durin_shader_build_private_dir}/ShaderCompileUtilities.cpp
		${_durin_shader_build_private_dir}/ShaderDependencyManifestStore.cpp
		${_durin_shader_build_private_dir}/SlangShaderDependencyResolver.cpp
	LIBRARIES Core RHI RenderCore DerivedDataCache ShaderBuild Slang_Imported
	INCLUDE_DIRECTORIES "${DURIN_PROJECT_SOURCE_DIR}/Developer/DerivedDataCache/Private" "${_durin_render_core_private_dir}" "${_durin_shader_build_private_dir}"
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
	TIMEOUT 600
	PRIVATE_SOURCE_OWNER ShaderBuild
)

durin_add_native_test(RenderShaderBuilderTests
	${_durin_shader_compiler_options}
	KIND feature
	DOMAINS shader
	MODULES rendercore shaderbuild derived-data-cache
	PRIVATE_SOURCE_RATIONALE
		"RenderCore-owned shader builder white-box coverage avoids exporting private builder implementation symbols."
	SOURCES Private/ShaderBuilderTests.cpp
	PRIVATE_SOURCES
		${_durin_shader_build_private_dir}/SlangSessionEnvironment.cpp
		${_durin_shader_build_private_dir}/SlangShaderCompiler.cpp
		${_durin_shader_build_private_dir}/ShaderCompileUtilities.cpp
		${_durin_shader_build_private_dir}/ShaderDependencyManifestStore.cpp
		${_durin_shader_build_private_dir}/ShaderBuilder.cpp
		${_durin_shader_build_private_dir}/ShaderBuilderAsync.cpp
		${_durin_shader_build_private_dir}/ShaderSourcePreparation.cpp
		${_durin_shader_build_private_dir}/ShaderOutputCache.cpp
		${_durin_shader_build_private_dir}/SlangShaderDependencyResolver.cpp
	LIBRARIES Core RHI RenderCore DerivedDataCache ShaderBuild Slang_Imported
	INCLUDE_DIRECTORIES "${_durin_render_core_private_dir}" "${_durin_shader_build_private_dir}"
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
	TIMEOUT 600
	PRIVATE_SOURCE_OWNER ShaderBuild
)

durin_add_native_test(ShaderBuildQualificationTests
	${_durin_shader_compiler_options}
	KIND qualification
	DOMAINS shader derived-data
	MODULES rendercore shaderbuild derived-data-cache
	PRIVATE_SOURCE_RATIONALE
		"Measures ShaderBuild-owned compiler, DDC and LRU lifetimes independently without exporting the private builder."
	SOURCES Private/ShaderBuildQualificationTests.cpp
	PRIVATE_SOURCES
		${_durin_shader_build_private_dir}/SlangSessionEnvironment.cpp
		${_durin_shader_build_private_dir}/SlangShaderCompiler.cpp
		${_durin_shader_build_private_dir}/ShaderCompileUtilities.cpp
		${_durin_shader_build_private_dir}/ShaderDependencyManifestStore.cpp
		${_durin_shader_build_private_dir}/ShaderBuilder.cpp
		${_durin_shader_build_private_dir}/ShaderBuilderAsync.cpp
		${_durin_shader_build_private_dir}/ShaderSourcePreparation.cpp
		${_durin_shader_build_private_dir}/ShaderOutputCache.cpp
		${_durin_shader_build_private_dir}/SlangShaderDependencyResolver.cpp
	LIBRARIES Core RHI RenderCore DerivedDataCache ShaderBuild Slang_Imported
	INCLUDE_DIRECTORIES "${_durin_render_core_private_dir}" "${_durin_shader_build_private_dir}"
	TIMEOUT 600
	PRIVATE_SOURCE_OWNER ShaderBuild
)

durin_add_native_test(RenderShaderCookIntegrationTests
	KIND integration
	DOMAINS shader asset-cook
	MODULES rendercore renderer shaderbuild
	SOURCES Private/ShaderCookIntegrationTests.cpp
	LIBRARIES Core RHI RenderCore DerivedDataCache ShaderBuild Slang_Imported
	INCLUDE_DIRECTORIES "${_durin_render_core_private_dir}" "${_durin_shader_build_private_dir}"
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
	TIMEOUT 600
	RUNTIME_ONLY_TARGETS Renderer
	RUNTIME_ONLY_RATIONALE
		"Loads Renderer by production module name before invoking Cook. ShaderBuild is linked."
	HEAVY_RUNTIME_RATIONALE "Loads Renderer so Cook can freeze the runtime Global Shader request inventory."
)
if(APPLE)
durin_add_native_test(MetalShaderCookIntegrationTests
	KIND integration
	REQUIRES editor
	REQUIREMENT_RATIONALE "Produces native MacOS Cooked shaders using editor-only ShaderBuild and the Metal toolchain."
	DOMAINS shader asset-cook
	MODULES rendercore renderer shaderbuild
	BACKENDS metal
	SOURCES Private/MetalShaderCookIntegrationTests.cpp
	LIBRARIES Core RHI RenderCore DerivedDataCache ShaderBuild Slang_Imported
	INCLUDE_DIRECTORIES "${_durin_render_core_private_dir}" "${_durin_shader_build_private_dir}"
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
	TIMEOUT 600
	RUNTIME_ONLY_TARGETS Renderer
	RUNTIME_ONLY_RATIONALE "Loads Renderer to freeze and load the complete binary MacOS Shader inventory."
	HEAVY_RUNTIME_RATIONALE "Runs the native Metal toolchain and verifies source-free Cooked Shader loading after irreversible producer shutdown."
)
else()
	durin_exclude_native_test_sources(
		RATIONALE "Native metallib production requires the macOS Metal toolchain."
		SOURCES Private/MetalShaderCookIntegrationTests.cpp)
endif()
else()
	durin_exclude_native_test_sources(
		RATIONALE "Authoring-only Shader compiler, cache, and builder coverage is excluded from the runtime-only Game closure."
		SOURCES Private/ShaderFoundationTests.cpp
			Private/ShaderReflectionTests.cpp
			Private/ShaderDerivedDataTests.cpp
			Private/ShaderBuilderTests.cpp
			Private/ShaderBuildQualificationTests.cpp
			Private/ShaderCookIntegrationTests.cpp
			Private/MetalShaderCookIntegrationTests.cpp
	)
endif()

durin_add_native_test(RenderContractTests
	KIND contract
	DOMAINS renderer
	MODULES rendercore rhi
	SOURCES
		Private/GeometrySubmissionTests.cpp
		Private/RenderResourceCreationTests.cpp
		Private/RenderResourceLifecycleTests.cpp
		Private/RDGTests.cpp
		Private/RenderTargetLayoutTests.cpp
		Private/RHITextureTests.cpp
		Private/TextureReferenceTests.cpp
		Private/TextureResourceLifetimeContractTests.cpp
	LIBRARIES Core RHI RenderCore
	INCLUDE_DIRECTORIES "${_durin_render_core_private_dir}" "${_durin_shader_build_private_dir}"
)

durin_add_native_test(RDGQualificationTests
	KIND qualification
	DOMAINS render-performance renderer
	MODULES rendercore rhi
	SOURCES Private/RDGQualificationTests.cpp
	LIBRARIES Core RHI RenderCore
	INCLUDE_DIRECTORIES "${_durin_render_core_private_dir}"
)
