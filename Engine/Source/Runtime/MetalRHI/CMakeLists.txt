add_durin_module(MetalRHI)

if(APPLE)
	enable_language(OBJCXX)
	target_sources(MetalRHI PRIVATE Private/MetalDynamicRHI.mm)
	set_source_files_properties(Private/MetalDynamicRHI.mm PROPERTIES
		SKIP_PRECOMPILE_HEADERS ON
		COMPILE_FLAGS "-fobjc-arc")
	find_library(METAL_FRAMEWORK Metal REQUIRED)
	find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
	target_link_libraries(MetalRHI PRIVATE
		${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
endif()
