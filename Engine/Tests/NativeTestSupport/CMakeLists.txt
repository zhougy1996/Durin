add_library(NativeTestSupport STATIC
	Private/NativeTestSupport.cpp
)

durin_target_apply_common_definitions(NativeTestSupport NativeTestSupport)

target_include_directories(NativeTestSupport
	PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Public"
)

target_link_libraries(NativeTestSupport
	PUBLIC Core GTest::gtest
)

set_target_properties(NativeTestSupport PROPERTIES
	FOLDER "Tests/Infrastructure"
)

if(APPLE)
	add_library(DurinNativeTestApplicationProtocol STATIC
		Private/MacOS/NativeTestApplicationProtocol.cpp
	)
	target_include_directories(DurinNativeTestApplicationProtocol PUBLIC
		"${CMAKE_CURRENT_SOURCE_DIR}/Private/MacOS"
	)
	target_compile_features(DurinNativeTestApplicationProtocol PUBLIC cxx_std_20)
	set_target_properties(DurinNativeTestApplicationProtocol PROPERTIES
		FOLDER "Tests/Infrastructure"
	)

	add_executable(DurinNativeTestApplicationController
		Private/MacOS/NativeTestApplicationController.cpp
	)
	target_link_libraries(DurinNativeTestApplicationController PRIVATE
		DurinNativeTestApplicationProtocol
	)
	set_target_properties(DurinNativeTestApplicationController PROPERTIES
		FOLDER "Tests/Infrastructure"
	)

	add_executable(DurinNativeTestApplicationProbe
		Private/MacOS/NativeTestApplicationProbe.cpp
	)
	target_compile_features(DurinNativeTestApplicationProbe PRIVATE cxx_std_20)
	set_target_properties(DurinNativeTestApplicationProbe PROPERTIES
		FOLDER "Tests/Infrastructure"
		RUNTIME_OUTPUT_DIRECTORY
			"/private/tmp/DurinNativeTestApplicationProbe/$<CONFIG>"
	)

	add_executable(DurinNativeTestApplicationHost MACOSX_BUNDLE
		Private/MacOS/NativeTestApplicationHost.cpp
	)
	target_link_libraries(DurinNativeTestApplicationHost PRIVATE
		DurinNativeTestApplicationProtocol
	)
	set_target_properties(DurinNativeTestApplicationHost PROPERTIES
		FOLDER "Tests/Infrastructure"
		MACOSX_BUNDLE_INFO_PLIST
			"${CMAKE_CURRENT_SOURCE_DIR}/Private/MacOS/NativeTestApplicationHost-Info.plist.in"
		MACOSX_BUNDLE_BUNDLE_NAME "Durin Native Test Application Host"
		MACOSX_BUNDLE_GUI_IDENTIFIER "com.durin.tests.application-host"
	)
	add_dependencies(DurinNativeTestApplicationController
		DurinNativeTestApplicationHost
		DurinNativeTestApplicationProbe)
endif()
