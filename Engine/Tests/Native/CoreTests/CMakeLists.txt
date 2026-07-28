include(GoogleTest)

function(durin_add_core_functional_test target_name)
	add_durin_test(${target_name} ${ARGN})
	set_target_properties(${target_name} PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
	)
	target_link_libraries(${target_name} PRIVATE Core)
	durin_test_deploy_target_binary(${target_name} Core)
	durin_test_deploy_directory_to_data(
		${target_name}
		"${CMAKE_CURRENT_SOURCE_DIR}/Data"
	)
	durin_test_deploy_files_to_data(
		${target_name}
		"${DURIN_PROJECT_ROOT_DIR}/Configs/TP_DurinEditor.yaml"
	)
	durin_discover_tests(${target_name})
endfunction()

durin_add_core_functional_test(CoreUtilityTests
	Private/ConsoleCommandTests.cpp
	Private/GuidTests.cpp
	Private/JsonTests.cpp
	Private/LoggerTests.cpp
	Private/NameTests.cpp
	Private/ProfilingTests.cpp
	Private/StringHelperTests.cpp
	Private/TransformTests.cpp
	Private/VersionTests.cpp
	Private/YamlTests.cpp
	Private/XxHashTests.cpp
)

durin_add_core_functional_test(CoreFileSystemTests
	Private/DerivedDataCacheTests.cpp
	Private/FileHelperTests.cpp
	Private/LexicalPathTests.cpp
	Private/PathsTests.cpp
	Private/ProjectTests.cpp
)

durin_add_core_functional_test(CoreConcurrencyTests
	Private/DelegateTests.cpp
	Private/ThreadingTests.cpp
)
