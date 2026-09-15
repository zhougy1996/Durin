durin_add_native_test(CoreObjectTests
	KIND contract
	DOMAINS reflection
	MODULES core-dobject
	SOURCES Private/ReflectionTypeTests.cpp
	LIBRARIES Core CoreDObject
)
durin_add_native_test(CoreObjectReplacementTests
	KIND contract
	DOMAINS reflection
	MODULES core-dobject
	SOURCES Private/ObjectGraphReplacementTests.cpp
	LIBRARIES Core CoreDObject
)
durin_add_native_test(CorePropertyChangeTests
	KIND contract
	DOMAINS reflection
	MODULES core-dobject
	SOURCES Private/PropertyChangeTests.cpp
	LIBRARIES Core CoreDObject
)
durin_add_native_test(CorePropertyValueSnapshotTests
	KIND contract
	DOMAINS reflection
	MODULES core-dobject
	SOURCES Private/ZPropertyValueSnapshotTests.cpp
	LIBRARIES Core CoreDObject
)
durin_add_native_test(PackageLinkerContractTests
	KIND contract
	DOMAINS reflection
	MODULES core-dobject
	SOURCES Private/PackageLinkerTests.cpp
	LIBRARIES Core CoreDObject
)
durin_add_native_test(PackageWriterContractTests
	KIND contract
	DOMAINS reflection
	MODULES core-dobject
	SOURCES Private/PackageWriterTests.cpp
	LIBRARIES Core CoreDObject
)
