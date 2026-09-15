durin_add_native_test(NativeTestApplicationExecutionTests
	KIND infrastructure
	EXECUTION_HOST application
	DOMAINS launch
	MODULES native-test-support
	STACKS process
	SOURCES Private/NativeTestApplicationExecutionTests.cpp
)
