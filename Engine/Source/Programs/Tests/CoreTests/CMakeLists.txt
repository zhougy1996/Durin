include(GoogleTest)

add_durin_test(CoreTests
	Private/ConsoleCommandTests.cpp
	Private/DelegateTests.cpp
	Private/DerivedDataCacheTests.cpp
	Private/GuidTests.cpp
	Private/JsonTests.cpp
	Private/LexicalPathTests.cpp
	Private/LoggerTests.cpp
	Private/NameTests.cpp
	Private/ProjectTests.cpp
	Private/PathsTests.cpp
	Private/StringHelperTests.cpp
	Private/ThreadingTests.cpp
	Private/TransformTests.cpp
	Private/VersionTests.cpp
	Private/YamlTests.cpp
	Private/XxHashTests.cpp
)

target_link_libraries(CoreTests PRIVATE
	Core
	GTest::gtest_main
)

durin_test_deploy_target_binary(CoreTests Core)
durin_test_deploy_directory_to_data(CoreTests "${CMAKE_CURRENT_SOURCE_DIR}/TestData")
durin_test_deploy_files_to_data(CoreTests "${DURIN_PROJECT_ROOT_DIR}/Configs/TP_DurinEditor.yaml")

get_target_property(_durin_core_tests_work_dir CoreTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(CoreTests
	WORKING_DIRECTORY "${_durin_core_tests_work_dir}"
)
