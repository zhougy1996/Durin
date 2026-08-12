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
durin_finalize_native_test(RHICommandListTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
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
durin_finalize_native_test(RHIThreadTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
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
durin_finalize_native_test(RHIInitializationTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
)
durin_discover_tests(RHIInitializationTests)

add_durin_test(RHIResourceTransitionValidationTests
	Private/RHIResourceTransitionValidationTests.cpp
)
target_link_libraries(RHIResourceTransitionValidationTests PRIVATE
	Core
	RHI
)
set_target_properties(RHIResourceTransitionValidationTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_finalize_native_test(RHIResourceTransitionValidationTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
)
durin_discover_tests(RHIResourceTransitionValidationTests)

add_durin_test(RHIResourceViewValidationTests
	Private/RHIResourceViewValidationTests.cpp
)
target_link_libraries(RHIResourceViewValidationTests PRIVATE
	Core
	RHI
)
set_target_properties(RHIResourceViewValidationTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_finalize_native_test(RHIResourceViewValidationTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
)
durin_discover_tests(RHIResourceViewValidationTests)

add_durin_test(RHITransferValidationTests
	Private/RHITransferValidationTests.cpp
)
target_link_libraries(RHITransferValidationTests PRIVATE
	Core
	RHI
)
set_target_properties(RHITransferValidationTests PROPERTIES
	DURIN_TEST_CASE_PARALLEL_SAFE TRUE
)
durin_finalize_native_test(RHITransferValidationTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
)
durin_discover_tests(RHITransferValidationTests)
