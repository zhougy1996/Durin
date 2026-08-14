include(GoogleTest)

function(durin_add_core_functional_test target_name)
	add_durin_test(${target_name} ${ARGN})
	set_target_properties(${target_name} PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
	)
	target_link_libraries(${target_name} PRIVATE Core)
	durin_test_deploy_directory_to_data(
		${target_name}
		"${CMAKE_CURRENT_SOURCE_DIR}/Data"
	)
	durin_test_deploy_files_to_data(
		${target_name}
		"${DURIN_PROJECT_ROOT_DIR}/Configs/TP_DurinEditor.yaml"
	)
	durin_finalize_native_test(${target_name}
		KIND contract
		DOMAINS core
		MODULES core
	)
	durin_discover_tests(${target_name})
endfunction()

durin_add_core_functional_test(CoreUtilityTests
	Private/ArchiveTests.cpp
	Private/AssertionTests.cpp
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
set_target_properties(ImageCodecTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
target_link_libraries(ImageCodecTests PRIVATE Core)
durin_finalize_native_test(ImageCodecTests
	KIND contract
	DOMAINS image-codec
	MODULES core
)
durin_discover_tests(ImageCodecTests)

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
	Private/DerivedDataCacheTests.cpp
	Private/FileHelperTests.cpp
	Private/LexicalPathTests.cpp
	Private/PathsTests.cpp
	Private/ProjectTests.cpp
)

durin_add_core_functional_test(CoreConcurrencyTests
	Private/DelegateTests.cpp
	Private/ProcessCrashContextTests.cpp
	Private/ThreadingTests.cpp
)

add_durin_test(CoreConcurrencyQualificationTests
	Private/ThreadingQualificationTests.cpp
)
set_target_properties(CoreConcurrencyQualificationTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
target_link_libraries(CoreConcurrencyQualificationTests PRIVATE Core)
durin_finalize_native_test(CoreConcurrencyQualificationTests
	KIND qualification
	DOMAINS core
	MODULES core
)
durin_discover_tests(CoreConcurrencyQualificationTests)
