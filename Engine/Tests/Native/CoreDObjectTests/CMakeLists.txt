include(GoogleTest)

function(durin_add_core_object_functional_test target_name source)
	add_durin_test(${target_name} ${source})
	target_link_libraries(${target_name} PRIVATE
		Core
		CoreDObject
	)
	set_target_properties(${target_name} PROPERTIES
		DURIN_TEST_CASE_PARALLEL_SAFE TRUE
	)
	durin_finalize_native_test(${target_name}
		KIND contract
		DOMAINS reflection
		MODULES core-dobject
	)
	durin_discover_tests(${target_name})
endfunction()

durin_add_core_object_functional_test(
	CoreObjectTests Private/ReflectionTypeTests.cpp)
durin_add_core_object_functional_test(
	CorePropertyChangeTests Private/PropertyChangeTests.cpp)
durin_add_core_object_functional_test(
	CorePropertyValueSnapshotTests Private/ZPropertyValueSnapshotTests.cpp)
