include(GoogleTest)

set(_durin_render_core_private_dir
	"${DURIN_PROJECT_SOURCE_DIR}/Runtime/RenderCore/Private")
set(_durin_shader_build_private_dir
	"${DURIN_PROJECT_SOURCE_DIR}/Developer/ShaderBuild/Private")

function(durin_add_render_functional_test target_name)
	set(options USE_SLANG)
	set(one_value_args KIND PRIVATE_SOURCE_RATIONALE)
	set(multi_value_args DOMAINS MODULES SOURCES PRIVATE_SOURCES)
	cmake_parse_arguments(
		DURIN_RENDER_TEST
		"${options}"
		"${one_value_args}"
		"${multi_value_args}"
		${ARGN}
	)

	add_durin_test(${target_name}
		${DURIN_RENDER_TEST_SOURCES}
		${DURIN_RENDER_TEST_PRIVATE_SOURCES}
	)
	target_include_directories(${target_name} PRIVATE
		"${_durin_render_core_private_dir}"
		"${_durin_shader_build_private_dir}"
	)
	target_link_libraries(${target_name} PRIVATE
		Core
		RHI
		RenderCore
	)

	if(DURIN_RENDER_TEST_USE_SLANG)
		target_link_libraries(${target_name} PRIVATE
			DerivedDataCache ShaderBuild Slang_Imported)
		durin_test_deploy_directory_to_data(
			${target_name}
			"${CMAKE_CURRENT_SOURCE_DIR}/Data"
		)
		set(_durin_timeout_arg TIMEOUT 600)
	endif()

	if(DURIN_RENDER_TEST_PRIVATE_SOURCES)
		durin_register_native_test(${target_name}
			KIND ${DURIN_RENDER_TEST_KIND}
			DOMAINS ${DURIN_RENDER_TEST_DOMAINS}
			MODULES ${DURIN_RENDER_TEST_MODULES}
			${_durin_timeout_arg}
			PRIVATE_SOURCE_OWNER ShaderBuild
			PRIVATE_SOURCE_RATIONALE
				"${DURIN_RENDER_TEST_PRIVATE_SOURCE_RATIONALE}"
		)
	else()
		durin_register_native_test(${target_name}
			KIND ${DURIN_RENDER_TEST_KIND}
			DOMAINS ${DURIN_RENDER_TEST_DOMAINS}
			MODULES ${DURIN_RENDER_TEST_MODULES}
			${_durin_timeout_arg}
		)
	endif()
endfunction()

durin_add_render_functional_test(RenderShaderCookedLibraryTests
	KIND contract
	DOMAINS shader
	MODULES rendercore
	SOURCES Private/ShaderCookedLibraryTests.cpp
)

if(DURIN_WITH_EDITOR)
durin_add_render_functional_test(RenderShaderContractTests
	KIND contract
	DOMAINS shader
	MODULES rendercore shaderbuild
	PRIVATE_SOURCE_RATIONALE
		"RenderCore-owned shader compiler white-box coverage avoids exporting private compiler implementation symbols."
	USE_SLANG
	SOURCES
		Private/ShaderFoundationTests.cpp
		Private/ShaderReflectionTests.cpp
	PRIVATE_SOURCES
		${_durin_shader_build_private_dir}/SlangShaderCompiler.cpp
		${_durin_shader_build_private_dir}/ShaderCompileUtilities.cpp
		${_durin_shader_build_private_dir}/SlangShaderDependencyResolver.cpp
)
target_compile_definitions(RenderShaderContractTests PRIVATE
	DURIN_ENGINE_SHADER_SOURCE_DIR="${DURIN_PROJECT_ROOT_DIR}/Shaders/Slang"
)

durin_add_render_functional_test(RenderShaderCacheTests
	KIND feature
	DOMAINS shader
	MODULES rendercore shaderbuild derived-data-cache
	PRIVATE_SOURCE_RATIONALE
		"RenderCore-owned shader cache white-box coverage avoids exporting private cache implementation symbols."
	USE_SLANG
	SOURCES Private/ShaderDerivedDataTests.cpp
	PRIVATE_SOURCES
		${_durin_shader_build_private_dir}/SlangShaderCompiler.cpp
		${_durin_shader_build_private_dir}/ShaderCompileUtilities.cpp
		${_durin_shader_build_private_dir}/ShaderDependencyManifestStore.cpp
		${_durin_shader_build_private_dir}/ShaderDerivedData.cpp
		${_durin_shader_build_private_dir}/SlangShaderDependencyResolver.cpp
)

durin_add_render_functional_test(RenderShaderServiceTests
	KIND feature
	DOMAINS shader
	MODULES rendercore shaderbuild derived-data-cache
	PRIVATE_SOURCE_RATIONALE
		"RenderCore-owned shader service white-box coverage avoids exporting private service implementation symbols."
	USE_SLANG
	SOURCES Private/ShaderCompileServiceTests.cpp
	PRIVATE_SOURCES
		${_durin_shader_build_private_dir}/SlangShaderCompiler.cpp
		${_durin_shader_build_private_dir}/ShaderCompileUtilities.cpp
		${_durin_shader_build_private_dir}/ShaderDependencyManifestStore.cpp
		${_durin_shader_build_private_dir}/ShaderDerivedData.cpp
		${_durin_shader_build_private_dir}/ShaderCompileService.cpp
		${_durin_shader_build_private_dir}/SlangShaderDependencyResolver.cpp
)

durin_add_render_functional_test(RenderShaderCookIntegrationTests
	KIND integration
	DOMAINS shader asset-cook
	MODULES rendercore renderer shaderbuild
	USE_SLANG
	SOURCES Private/ShaderCookIntegrationTests.cpp
)
durin_test_register_runtime_only_dependencies(
	RenderShaderCookIntegrationTests
	RATIONALE
		"The integration test loads Renderer and ShaderBuild by production module name before invoking Cook."
	TARGETS Renderer ShaderBuild
)
durin_test_deploy_target_binary(RenderShaderCookIntegrationTests Renderer)
set_target_properties(RenderShaderCookIntegrationTests PROPERTIES
	DURIN_TEST_HEAVY_RUNTIME_RATIONALE
		"Loads Renderer so the production Cook provider can freeze the exact runtime Global Shader request inventory."
)
else()
	durin_exclude_native_test_sources(
		RATIONALE "Authoring-only Shader compiler, cache, and service coverage is excluded from the runtime-only Game closure."
		SOURCES
			Private/ShaderFoundationTests.cpp
			Private/ShaderReflectionTests.cpp
			Private/ShaderDerivedDataTests.cpp
			Private/ShaderCompileServiceTests.cpp
			Private/ShaderCookIntegrationTests.cpp
	)
endif()

durin_add_render_functional_test(RenderContractTests
	KIND contract
	DOMAINS renderer
	MODULES rendercore rhi
	SOURCES
		Private/RenderResourceCreationTests.cpp
		Private/RenderResourceLifecycleTests.cpp
		Private/RDGTests.cpp
		Private/RenderTargetLayoutTests.cpp
		Private/RHITextureTests.cpp
		Private/TextureReferenceTests.cpp
		Private/TextureResourceLifetimeContractTests.cpp
)

durin_add_render_functional_test(RDGParameterLayoutQualificationTests
	KIND qualification
	DOMAINS renderer
	MODULES rendercore rhi
	SOURCES Private/RDGParameterLayoutQualificationTests.cpp
)
