set(_durin_render_core_private_dir
	"${DURIN_PROJECT_SOURCE_DIR}/Runtime/RenderCore/Private")
set(_durin_shader_build_private_dir
	"${DURIN_PROJECT_SOURCE_DIR}/Developer/ShaderBuild/Private")

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
	COMPILE_DEFINITIONS DURIN_ENGINE_SHADER_SOURCE_DIR="${DURIN_PROJECT_ROOT_DIR}/Shaders/Slang"
)

durin_add_native_test(RenderShaderCacheTests
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
		${_durin_shader_build_private_dir}/ShaderDerivedData.cpp
		${_durin_shader_build_private_dir}/SlangShaderDependencyResolver.cpp
	LIBRARIES Core RHI RenderCore DerivedDataCache ShaderBuild Slang_Imported
	INCLUDE_DIRECTORIES "${_durin_render_core_private_dir}" "${_durin_shader_build_private_dir}"
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
	TIMEOUT 600
	PRIVATE_SOURCE_OWNER ShaderBuild
)

durin_add_native_test(RenderShaderServiceTests
	KIND feature
	DOMAINS shader
	MODULES rendercore shaderbuild derived-data-cache
	PRIVATE_SOURCE_RATIONALE
		"RenderCore-owned shader service white-box coverage avoids exporting private service implementation symbols."
	SOURCES Private/ShaderCompileServiceTests.cpp
	PRIVATE_SOURCES
		${_durin_shader_build_private_dir}/SlangSessionEnvironment.cpp
		${_durin_shader_build_private_dir}/SlangShaderCompiler.cpp
		${_durin_shader_build_private_dir}/ShaderCompileUtilities.cpp
		${_durin_shader_build_private_dir}/ShaderDependencyManifestStore.cpp
		${_durin_shader_build_private_dir}/ShaderDerivedData.cpp
		${_durin_shader_build_private_dir}/ShaderCompileService.cpp
		${_durin_shader_build_private_dir}/SlangShaderDependencyResolver.cpp
	LIBRARIES Core RHI RenderCore DerivedDataCache ShaderBuild Slang_Imported
	INCLUDE_DIRECTORIES "${_durin_render_core_private_dir}" "${_durin_shader_build_private_dir}"
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
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
else()
	durin_exclude_native_test_sources(
		RATIONALE "Authoring-only Shader compiler, cache, and service coverage is excluded from the runtime-only Game closure."
		SOURCES Private/ShaderFoundationTests.cpp
			Private/ShaderReflectionTests.cpp
			Private/ShaderDerivedDataTests.cpp
			Private/ShaderCompileServiceTests.cpp
			Private/ShaderCookIntegrationTests.cpp
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
