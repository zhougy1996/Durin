include(GoogleTest)

add_durin_test(CoreDObjectTests
	Private/ReflectionTypeTests.cpp
)

target_link_libraries(CoreDObjectTests PRIVATE
	Core
	CoreDObject
	GTest::gtest_main
)

durin_test_deploy_target_binary(CoreDObjectTests Core)
durin_test_deploy_target_binary(CoreDObjectTests CoreDObject)

get_target_property(_durin_core_dobject_tests_work_dir CoreDObjectTests DURIN_TEST_WORK_DIR)

gtest_discover_tests(CoreDObjectTests
	WORKING_DIRECTORY "${_durin_core_dobject_tests_work_dir}"
)
