include(GoogleTest)

add_durin_test(CoreDObjectTests
	Private/PropertyChangeTests.cpp
	Private/ReflectionTypeTests.cpp
	Private/ZPropertyValueSnapshotTests.cpp
)

target_link_libraries(CoreDObjectTests PRIVATE
	Core
	CoreDObject
	GTest::gtest_main
)

durin_test_deploy_target_binary(CoreDObjectTests Core)
durin_test_deploy_target_binary(CoreDObjectTests CoreDObject)

durin_discover_tests(CoreDObjectTests)
