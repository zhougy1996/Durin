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
