include(GoogleTest)

add_durin_test(CoreObjectTests
	Private/PropertyChangeTests.cpp
	Private/ReflectionTypeTests.cpp
	Private/ZPropertyValueSnapshotTests.cpp
)

target_link_libraries(CoreObjectTests PRIVATE
	Core
	CoreDObject
)

durin_test_deploy_target_binary(CoreObjectTests Core)
durin_test_deploy_target_binary(CoreObjectTests CoreDObject)

set_target_properties(CoreObjectTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)

durin_discover_tests(CoreObjectTests)
