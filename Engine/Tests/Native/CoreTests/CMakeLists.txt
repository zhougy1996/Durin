include(GoogleTest)

function(durin_add_core_functional_test target_name)
	add_durin_test(${target_name} ${ARGN})
	target_link_libraries(${target_name} PRIVATE Core)
	durin_test_deploy_directory_to_data(
		${target_name}
		"${CMAKE_CURRENT_SOURCE_DIR}/Data"
	)
	durin_test_deploy_files_to_data(
		${target_name}
		"${DURIN_PROJECT_ROOT_DIR}/Configs/TP_DurinEditor.yaml"
	)
	durin_register_native_test(${target_name}
		KIND contract
		DOMAINS core
		MODULES core
	)
endfunction()

durin_add_core_functional_test(CoreUtilityTests
	Private/ArchiveTests.cpp
	Private/AssertionTests.cpp
	Private/CheckedArithmeticTests.cpp
	Private/ConsoleCommandTests.cpp
	Private/GuidTests.cpp
	Private/JsonTests.cpp
	Private/LoggerTests.cpp
	Private/MathFacadeTests.cpp
	Private/NameTests.cpp
	Private/ProfilingTests.cpp
	Private/StringHelperTests.cpp
	Private/StartupCommandTests.cpp
	Private/TransformTests.cpp
	Private/VersionTests.cpp
	Private/YamlTests.cpp
	Private/XxHashTests.cpp
)

add_durin_test(ImageCodecTests Private/ImageDecoderTests.cpp)
target_link_libraries(ImageCodecTests PRIVATE Core)
durin_register_native_test(ImageCodecTests
	KIND contract
	DOMAINS image-codec
	MODULES core
)

set(_durin_assertion_fixture_dir
	"${DURIN_WORKSPACE_DIR}/CMake/Tests/Fixtures/AssertionHeader")
target_include_directories(CoreUtilityTests PRIVATE
	"${_durin_assertion_fixture_dir}")

foreach(_durin_assertion_fixture_target IN ITEMS
	CoreAssertionHeaderNoPCH
	CoreAssertionHeaderSharedPCH)
	add_library(${_durin_assertion_fixture_target} OBJECT
		"${_durin_assertion_fixture_dir}/AssertionHeaderFixture.cpp")
	durin_target_apply_runtime_variant_definitions(
		${_durin_assertion_fixture_target})
	target_link_libraries(${_durin_assertion_fixture_target} PRIVATE Core)
	set_target_properties(${_durin_assertion_fixture_target} PROPERTIES
		EXCLUDE_FROM_ALL TRUE
		FOLDER "Tests/CompileFixtures")
endforeach()

add_library(CoreAssertionHeaderModuleConsumer OBJECT
	"${_durin_assertion_fixture_dir}/AssertionHeaderFixture.cpp")
durin_target_apply_common_definitions(
	CoreAssertionHeaderModuleConsumer AssertionFixtureConsumer)
target_link_libraries(CoreAssertionHeaderModuleConsumer PRIVATE Core)
set_target_properties(CoreAssertionHeaderModuleConsumer PROPERTIES
	EXCLUDE_FROM_ALL TRUE
	FOLDER "Tests/CompileFixtures")

if(DURIN_ENABLE_PCH AND TARGET SharedPCH_Core)
	target_precompile_headers(CoreAssertionHeaderSharedPCH
		REUSE_FROM SharedPCH_Core)
endif()

add_dependencies(CoreUtilityTests
	CoreAssertionHeaderNoPCH
	CoreAssertionHeaderSharedPCH
	CoreAssertionHeaderModuleConsumer)

durin_add_core_functional_test(CoreFileSystemTests
	Private/BinaryFormatTests.cpp
	Private/FileHelperTests.cpp
	Private/LexicalPathTests.cpp
	Private/PathsTests.cpp
	Private/ProjectTests.cpp
)

durin_add_core_functional_test(CoreConcurrencyTests
	Private/AsyncOperationGroupTests.cpp
	Private/DelegateTests.cpp
	Private/ModularFeatureTests.cpp
	Private/ProcessCrashContextTests.cpp
	Private/ThreadingTests.cpp
)

add_durin_test(CoreConcurrencyQualificationTests
	Private/ThreadingQualificationTests.cpp
)
target_link_libraries(CoreConcurrencyQualificationTests PRIVATE Core)
durin_register_native_test(CoreConcurrencyQualificationTests
	KIND qualification
	DOMAINS core
	MODULES core
)

if(WIN32 OR APPLE)
	set(_durin_dynamic_unload_fixture_dir
		"${DURIN_PROJECT_ROOT_DIR}/Tests/Fixtures/DynamicDllUnload")
	add_library(DynamicUnloadFixture MODULE
		"${_durin_dynamic_unload_fixture_dir}/DynamicUnloadFixtureModule.cpp"
	)
	durin_target_apply_common_definitions(
		DynamicUnloadFixture DynamicUnloadFixture)
	target_include_directories(DynamicUnloadFixture PRIVATE
		"${_durin_dynamic_unload_fixture_dir}"
	)
	target_link_libraries(DynamicUnloadFixture PRIVATE Core)
	set_target_properties(DynamicUnloadFixture PROPERTIES
		OUTPUT_NAME "${DURIN_RUNTIME_VARIANT}-DynamicUnloadFixture"
		FOLDER "Tests/Fixtures"
	)
	if(APPLE)
		set_target_properties(DynamicUnloadFixture PROPERTIES SUFFIX ".dylib")
	endif()
	durin_target_set_runtime_outputs(DynamicUnloadFixture)
	if(DURIN_ENABLE_PCH AND TARGET SharedPCH_Core)
		target_precompile_headers(DynamicUnloadFixture REUSE_FROM SharedPCH_Core)
	endif()

	add_durin_test(DynamicDllUnloadQualificationTests
		Private/DynamicDllUnloadQualificationTests.cpp
	)
	target_include_directories(DynamicDllUnloadQualificationTests PRIVATE
		"${_durin_dynamic_unload_fixture_dir}"
	)
	target_link_libraries(DynamicDllUnloadQualificationTests PRIVATE Core)
	durin_test_register_runtime_only_dependencies(
		DynamicDllUnloadQualificationTests
		RATIONALE
			"The production module manager delay-loads the real unload fixture DLL by module name."
		TARGETS DynamicUnloadFixture
	)
	durin_register_native_test(DynamicDllUnloadQualificationTests
		KIND qualification
		DOMAINS core
		MODULES core
		STACKS process
	)

	add_durin_test(DynamicDllUnloadFailureQualificationTests
		Private/DynamicDllUnloadFailureQualificationTests.cpp
	)
	target_include_directories(DynamicDllUnloadFailureQualificationTests PRIVATE
		"${_durin_dynamic_unload_fixture_dir}"
	)
	target_link_libraries(DynamicDllUnloadFailureQualificationTests PRIVATE Core)
	durin_test_register_runtime_only_dependencies(
		DynamicDllUnloadFailureQualificationTests
		RATIONALE
			"The production module manager delay-loads the real failure-injection fixture DLL by module name."
		TARGETS DynamicUnloadFixture
	)
	durin_register_native_test(DynamicDllUnloadFailureQualificationTests
		KIND qualification
		DOMAINS core
		MODULES core
		STACKS process
	)
else()
	durin_exclude_native_test_sources(
		RATIONALE
			"Physical dynamic-library unload qualification currently requires the Windows DLL fixture."
		SOURCES
			Private/DynamicDllUnloadQualificationTests.cpp
			Private/DynamicDllUnloadFailureQualificationTests.cpp
	)
endif()
