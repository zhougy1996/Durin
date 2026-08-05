include(GoogleTest)

add_durin_test(RHICommandListTests
	Private/RHICommandListTests.cpp
)
target_link_libraries(RHICommandListTests PRIVATE
	Core
	RHI
)
set_target_properties(RHICommandListTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_discover_tests(RHICommandListTests)

add_durin_test(RHIThreadTests
	Private/RHIThreadTests.cpp
)
target_link_libraries(RHIThreadTests PRIVATE
	Core
	RHI
)
set_target_properties(RHIThreadTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_discover_tests(RHIThreadTests)

add_durin_test(RHIInitializationTests
	Private/RHIInitializationTests.cpp
)
target_link_libraries(RHIInitializationTests PRIVATE
	Core
	RHI
)
set_target_properties(RHIInitializationTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_discover_tests(RHIInitializationTests)
