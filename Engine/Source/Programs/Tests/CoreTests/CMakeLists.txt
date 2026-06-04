include(GoogleTest)

add_durin_test(CoreTests
	Private/JsonTests.cpp
	Private/ThreadingTests.cpp
	Private/YamlTests.cpp
	Private/XxHashTests.cpp
)

target_compile_definitions(CoreTests PRIVATE
	CORE_TEST_SAMPLE_JSON_FILE="${CMAKE_CURRENT_SOURCE_DIR}/TestData/Sample.json"
	CORE_TEST_SAMPLE_YAML_FILE="${CMAKE_CURRENT_SOURCE_DIR}/TestData/Sample.yaml"
	CORE_TEST_SAMPLE_APP_CONFIG_FILE="${DURIN_PROJECT_ROOT_DIR}/Configs/TP_DurinEditor.yaml"
)

target_link_libraries(CoreTests PRIVATE
	Core
	GTest::gtest_main
)

durin_target_copy_binary_to_output_dir(CoreTests Core)

gtest_discover_tests(CoreTests
	WORKING_DIRECTORY $<TARGET_FILE_DIR:CoreTests>
)
