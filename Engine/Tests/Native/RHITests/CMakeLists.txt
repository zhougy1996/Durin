durin_add_native_test(RHIPipelineCreationTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
	SOURCES Private/RHIPipelineCreationTests.cpp
	LIBRARIES Core RHI
)

durin_add_native_test(RHICommandListTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
	SOURCES Private/RHICommandListTests.cpp Private/RHICompletionTests.cpp
	LIBRARIES Core RHI
)

durin_add_native_test(RHIThreadTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
	SOURCES Private/RHIThreadTests.cpp
	LIBRARIES Core RHI
)

durin_add_native_test(RHIInitializationTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
	SOURCES Private/RHIInitializationTests.cpp
	LIBRARIES Core RHI
)

durin_add_native_test(RHIResourceTransitionValidationTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
	SOURCES Private/RHIResourceTransitionValidationTests.cpp
	LIBRARIES Core RHI
)

durin_add_native_test(RHIResourceViewValidationTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
	SOURCES Private/RHIResourceViewValidationTests.cpp
	LIBRARIES Core RHI
)

durin_add_native_test(RHITransferValidationTests
	KIND contract
	DOMAINS renderer
	MODULES rhi
	SOURCES Private/RHITransferValidationTests.cpp
	LIBRARIES Core RHI
)
