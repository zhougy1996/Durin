durin_add_native_test(NativeTestApplicationHostTests
	KIND infrastructure
	DOMAINS launch
	MODULES native-test-support
	STACKS process
	SOURCES Private/NativeTestApplicationProtocolTests.cpp Private/NativeTestApplicationControllerTests.cpp
	LIBRARIES DurinNativeTestApplicationProtocol
	COMPILE_DEFINITIONS
		DURIN_APPLICATION_TEST_CONTROLLER="$<TARGET_FILE:DurinNativeTestApplicationController>"
		DURIN_APPLICATION_TEST_HOST_BUNDLE="$<TARGET_BUNDLE_DIR:DurinNativeTestApplicationHost>"
		DURIN_APPLICATION_TEST_PROBE="$<TARGET_FILE:DurinNativeTestApplicationProbe>"
	DEPENDENCIES
		DurinNativeTestApplicationController
		DurinNativeTestApplicationHost
		DurinNativeTestApplicationProbe
)
