include(GoogleTest)

set(_durin_render_core_private_dir
	"${DURIN_PROJECT_SOURCE_DIR}/Runtime/RenderCore/Private")

function(durin_add_render_functional_test target_name)
	set(options USE_SLANG)
	set(one_value_args)
	set(multi_value_args SOURCES PRIVATE_SOURCES)
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
	)
	target_link_libraries(${target_name} PRIVATE
		Core
		RHI
		RenderCore
	)
	durin_test_deploy_target_binary(${target_name} Core)
	durin_test_deploy_target_binary(${target_name} RHI)
	durin_test_deploy_target_binary(${target_name} RenderCore)

	if(DURIN_RENDER_TEST_USE_SLANG)
		target_link_libraries(${target_name} PRIVATE Slang_Imported)
		durin_test_deploy_runtime_files(${target_name} Slang_Imported)
		durin_test_deploy_directory_to_data(
			${target_name}
			"${CMAKE_CURRENT_SOURCE_DIR}/Data"
		)
		set_target_properties(${target_name} PROPERTIES
			DURIN_TEST_TIMEOUT 600
		)
	endif()

	set_target_properties(${target_name} PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
	)
	durin_discover_tests(${target_name})
endfunction()

durin_add_render_functional_test(RenderShaderContractTests
	USE_SLANG
	SOURCES
		Private/ShaderFoundationTests.cpp
		Private/ShaderReflectionTests.cpp
	PRIVATE_SOURCES
		${_durin_render_core_private_dir}/Shader/SlangShaderCompiler.cpp
		${_durin_render_core_private_dir}/Shader/ShaderCompileUtilities.cpp
		${_durin_render_core_private_dir}/Shader/SlangShaderDependencyResolver.cpp
)

durin_add_render_functional_test(RenderShaderCacheTests
	USE_SLANG
	SOURCES Private/ShaderCacheStoreTests.cpp
	PRIVATE_SOURCES
		${_durin_render_core_private_dir}/Shader/SlangShaderCompiler.cpp
		${_durin_render_core_private_dir}/Shader/ShaderCompileUtilities.cpp
		${_durin_render_core_private_dir}/Shader/ShaderCacheStore.cpp
		${_durin_render_core_private_dir}/Shader/SlangShaderDependencyResolver.cpp
)

durin_add_render_functional_test(RenderShaderServiceTests
	USE_SLANG
	SOURCES Private/ShaderCompileServiceTests.cpp
	PRIVATE_SOURCES
		${_durin_render_core_private_dir}/Shader/SlangShaderCompiler.cpp
		${_durin_render_core_private_dir}/Shader/ShaderCompileUtilities.cpp
		${_durin_render_core_private_dir}/Shader/ShaderCacheStore.cpp
		${_durin_render_core_private_dir}/Shader/ShaderCompileService.cpp
		${_durin_render_core_private_dir}/Shader/SlangShaderDependencyResolver.cpp
)

durin_add_render_functional_test(RenderContractTests
	SOURCES
		Private/RenderResourceLifecycleTests.cpp
		Private/RenderTargetLayoutTests.cpp
		Private/RHITextureTests.cpp
		Private/TextureReferenceTests.cpp
		Private/TextureResourceLifetimeContractTests.cpp
)
