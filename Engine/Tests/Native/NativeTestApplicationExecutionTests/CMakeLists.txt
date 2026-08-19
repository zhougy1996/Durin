include(GoogleTest)

add_durin_test(NativeTestApplicationExecutionTests
	Private/NativeTestApplicationExecutionTests.cpp
)
durin_register_native_test(NativeTestApplicationExecutionTests
	KIND infrastructure
	EXECUTION_HOST application
	DOMAINS launch
	MODULES native-test-support
	STACKS process
)
