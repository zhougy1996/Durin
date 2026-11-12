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

# Shared registration object: a static library could discard its initializer
# because test executables reference no symbols from this translation unit.
if(DURIN_WITH_EDITOR)
	add_library(AuthoredShaderTestEnvironment OBJECT
		Private/AuthoredShaderTestEnvironment.cpp
	)
	durin_target_apply_common_definitions(AuthoredShaderTestEnvironment AuthoredShaderTestEnvironment)
	target_link_libraries(AuthoredShaderTestEnvironment PRIVATE Core GTest::gtest)
	set_target_properties(AuthoredShaderTestEnvironment PROPERTIES FOLDER "Tests/Infrastructure")
endif()

if(APPLE AND DURIN_ENABLE_APPLICATION_TESTS)
	add_library(DurinNativeTestApplicationProtocol STATIC
		Private/MacOS/NativeTestApplicationProtocol.cpp
	)
	target_include_directories(DurinNativeTestApplicationProtocol PUBLIC
		"${CMAKE_CURRENT_SOURCE_DIR}/Private/MacOS"
	)
	target_compile_features(DurinNativeTestApplicationProtocol PUBLIC cxx_std_23)
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
	target_compile_features(DurinNativeTestApplicationProbe PRIVATE cxx_std_23)
	set_target_properties(DurinNativeTestApplicationProbe PROPERTIES
		FOLDER "Tests/Infrastructure"
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
endif()

add_library(VulkanRHITestSupport STATIC EXCLUDE_FROM_ALL
	Private/VulkanTestSupport.cpp
)
durin_target_apply_common_definitions(VulkanRHITestSupport VulkanRHITestSupport)
target_include_directories(VulkanRHITestSupport PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/Public"
	"${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/VulkanRHI/Private"
)
target_link_libraries(VulkanRHITestSupport PUBLIC Core RHI RenderCore VulkanRHI Vulkan::Vulkan)
set_target_properties(VulkanRHITestSupport PROPERTIES FOLDER "Tests/Infrastructure")
