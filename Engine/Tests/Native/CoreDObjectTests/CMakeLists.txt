include(GoogleTest)

add_durin_test(CoreObjectTests
	Private/PropertyChangeTests.cpp
	Private/ReflectionTypeTests.cpp
	Private/ZPropertyValueSnapshotTests.cpp
)

target_link_libraries(CoreObjectTests PRIVATE
	Core
	CoreDObject
	GTest::gtest_main
)

durin_test_deploy_target_binary(CoreObjectTests Core)
durin_test_deploy_target_binary(CoreObjectTests CoreDObject)

set_target_properties(CoreObjectTests PROPERTIES
	DURIN_TEST_LEGACY_SERIALIZATION_GROUP object-lifecycle
)

durin_discover_tests(CoreObjectTests)
