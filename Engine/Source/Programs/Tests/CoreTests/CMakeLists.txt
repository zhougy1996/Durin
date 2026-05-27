include(GoogleTest)

durin_add_test_target(CoreTests
	Private/JsonTests.cpp
)

target_compile_definitions(CoreTests PRIVATE
	CORE_TEST_SAMPLE_JSON_FILE="${CMAKE_CURRENT_SOURCE_DIR}/TestData/Sample.json"
)

target_link_libraries(CoreTests PRIVATE
	Core
	GTest::gtest_main
)

gtest_discover_tests(CoreTests
	WORKING_DIRECTORY $<TARGET_FILE_DIR:CoreTests>
)
