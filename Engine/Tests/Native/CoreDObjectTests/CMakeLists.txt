include(GoogleTest)

function(durin_add_core_object_functional_test target_name source)
	add_durin_test(${target_name} ${source})
	target_link_libraries(${target_name} PRIVATE
		Core
		CoreDObject
	)
	durin_register_native_test(${target_name}
		KIND contract
		DOMAINS reflection
		MODULES core-dobject
	)
endfunction()

durin_add_core_object_functional_test(
	CoreObjectTests Private/ReflectionTypeTests.cpp)
durin_add_core_object_functional_test(
	CorePropertyChangeTests Private/PropertyChangeTests.cpp)
durin_add_core_object_functional_test(
	CorePropertyValueSnapshotTests Private/ZPropertyValueSnapshotTests.cpp)
durin_add_core_object_functional_test(
	PackageLinkerContractTests Private/PackageLinkerTests.cpp)
durin_add_core_object_functional_test(
	PackageWriterContractTests Private/PackageWriterTests.cpp)
