set(_function_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/FunctionFixture")
file(GLOB_RECURSE _function_fixture_dht_sources CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/Engine/Source/Programs/DurinHeaderTool/durin_header_tool/*.py")
add_custom_command(
	OUTPUT "${_function_fixture_dir}/FunctionReflectionTestTypes.gen.h"
		"${_function_fixture_dir}/FunctionReflectionTestTypes.gen.cpp"
	COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/GenerateFunctionFixture.py" "${_function_fixture_dir}"
	DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/GenerateFunctionFixture.py"
		"${CMAKE_CURRENT_SOURCE_DIR}/Private/FunctionReflectionTestTypes.h" ${_function_fixture_dht_sources}
	VERBATIM
)
durin_add_native_test(CoreTransactionContractTests
	KIND contract
	DOMAINS reflection
	MODULES core core-dobject
	SOURCES Private/TransactionContractTests.cpp Private/TransactionSerializationTests.cpp
	LIBRARIES Core CoreDObject
)

durin_add_native_test(CoreObjectTests
	KIND contract
	DOMAINS reflection
	MODULES core-dobject
	SOURCES Private/ReflectionTypeTests.cpp Private/FunctionReflectionTests.cpp Private/ReflectionRegressionTests.cpp
	LIBRARIES Core CoreDObject
)
target_sources(CoreObjectTests PRIVATE "${_function_fixture_dir}/FunctionReflectionTestTypes.gen.cpp")
target_include_directories(CoreObjectTests PRIVATE "${_function_fixture_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/Private")
target_compile_definitions(CoreObjectTests PRIVATE FUNCTIONFIXTURE_API=)
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
durin_add_native_test(PackagePersistenceTests
	KIND feature
	DOMAINS reflection
	MODULES core-dobject
	SOURCES Private/PackagePersistenceTests.cpp
	LIBRARIES Core CoreDObject
)
