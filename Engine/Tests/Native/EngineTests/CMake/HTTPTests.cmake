if(NOT TARGET HTTPServer)
	durin_exclude_native_test_sources(
		RATIONALE "HTTPServer is opt-in and is not selected by this runtime's module dependencies."
		SOURCES Private/HTTPServerLifecycleTests.cpp)
	return()
endif()

durin_add_native_test(HTTPServerLifecycleTests
	KIND integration
	DOMAINS http-server
	MODULES http-server
	SOURCES Private/HTTPServerLifecycleTests.cpp
	LIBRARIES Core HTTPServer httplib::httplib
)
set_source_files_properties(Private/HTTPServerLifecycleTests.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
