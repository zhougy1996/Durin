set(_durin_assertion_fixture_dir
	"${DURIN_WORKSPACE_DIR}/CMake/Tests/Fixtures/AssertionHeader")

durin_add_native_test(CoreUtilityTests
	KIND contract
	DOMAINS core
	MODULES core
	SOURCES
		Private/ArchiveTests.cpp
		Private/AssertionTests.cpp
		Private/BinaryEnvelopeTests.cpp
		Private/CheckedArithmeticTests.cpp
		Private/ColorTests.cpp
		Private/ConsoleCommandTests.cpp
		Private/GuidTests.cpp
		Private/JsonTests.cpp
		Private/LoggerTests.cpp
		Private/MathFacadeTests.cpp
		Private/NameTests.cpp
		Private/ProfilingTests.cpp
		Private/StringHelperTests.cpp
		Private/StartupCommandTests.cpp
		Private/TimeTests.cpp
		Private/TransformTests.cpp
		Private/YamlTests.cpp
		Private/XxHashTests.cpp
	LIBRARIES Core
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
	DATA_FILES "${DURIN_PROJECT_ROOT_DIR}/Configs/TP_DurinEditor.yaml"
	INCLUDE_DIRECTORIES "${_durin_assertion_fixture_dir}"
	DEPENDENCIES CoreAssertionHeaderNoPCH CoreAssertionHeaderSharedPCH CoreAssertionHeaderModuleConsumer
)

durin_add_native_test(ImageCodecTests
	KIND contract
	DOMAINS image-codec
	MODULES core
	SOURCES Private/ImageDecoderTests.cpp
	LIBRARIES Core
)

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

durin_add_native_test(CoreFileSystemTests
	KIND contract
	DOMAINS core
	MODULES core
	SOURCES
		Private/BinaryFormatTests.cpp
		Private/FileHelperTests.cpp
		Private/LexicalPathTests.cpp
		Private/PathsTests.cpp
		Private/ProjectTests.cpp
	LIBRARIES Core
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
	DATA_FILES "${DURIN_PROJECT_ROOT_DIR}/Configs/TP_DurinEditor.yaml"
)

durin_add_native_test(CoreConcurrencyTests
	KIND contract
	DOMAINS core
	MODULES core
	SOURCES
		Private/AsyncOperationGroupTests.cpp
		Private/AtomicSharedPtrTests.cpp
		Private/DelegateTests.cpp
		Private/ModularFeatureTests.cpp
		Private/ProcessCrashContextTests.cpp
		Private/ThreadingTests.cpp
		Private/TaskCompositionTests.cpp
	LIBRARIES Core
	DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
	DATA_FILES "${DURIN_PROJECT_ROOT_DIR}/Configs/TP_DurinEditor.yaml"
)

durin_add_native_test(CoreConcurrencyQualificationTests
	KIND qualification
	DOMAINS core
	MODULES core
	SOURCES Private/ThreadingQualificationTests.cpp
	LIBRARIES Core
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

	durin_add_native_test(DynamicDllUnloadQualificationTests
		KIND qualification
		DOMAINS core
		MODULES core
		STACKS process
		SOURCES Private/DynamicDllUnloadQualificationTests.cpp
		INCLUDE_DIRECTORIES "${_durin_dynamic_unload_fixture_dir}"
		LIBRARIES Core
		RUNTIME_ONLY_RATIONALE
			"The production module manager delay-loads the real unload fixture DLL by module name."
		RUNTIME_ONLY_TARGETS DynamicUnloadFixture
	)

	durin_add_native_test(DynamicDllUnloadFailureQualificationTests
		KIND qualification
		DOMAINS core
		MODULES core
		STACKS process
		SOURCES Private/DynamicDllUnloadFailureQualificationTests.cpp
		INCLUDE_DIRECTORIES "${_durin_dynamic_unload_fixture_dir}"
		LIBRARIES Core
		RUNTIME_ONLY_RATIONALE
			"The production module manager delay-loads the real failure-injection fixture DLL by module name."
		RUNTIME_ONLY_TARGETS DynamicUnloadFixture
	)
else()
	durin_exclude_native_test_sources(
		RATIONALE
			"Physical dynamic-library unload qualification currently requires the Windows DLL fixture."
		SOURCES Private/DynamicDllUnloadQualificationTests.cpp
			Private/DynamicDllUnloadFailureQualificationTests.cpp
	)
endif()
