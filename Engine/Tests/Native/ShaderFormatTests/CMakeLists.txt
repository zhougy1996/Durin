durin_add_native_test(ShaderFormatTests
	KIND contract
	DOMAINS shader
	MODULES shader-format
	SOURCES Private/ShaderFormatTests.cpp
	LIBRARIES Core RHI ShaderFormat
)

durin_add_native_test(MetalBindingPlanTests
	KIND contract
	DOMAINS shader rhi-creation
	MODULES rhi shader-format
	SOURCES Private/MetalBindingPlanTests.cpp
	LIBRARIES Core RHI ShaderFormat
	INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/MetalRHI/Private"
		"${CMAKE_SOURCE_DIR}/Engine/Tests/Native/MetalRHITests/Private"
)
