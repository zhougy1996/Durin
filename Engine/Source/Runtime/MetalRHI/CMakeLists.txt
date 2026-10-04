add_durin_module(MetalRHI)

if(APPLE)
	enable_language(OBJCXX)
	set(METAL_CPP_SOURCE_DIR "${DURIN_PROJECT_ROOT_DIR}/External/Source/metal-cpp")
	if(NOT EXISTS "${METAL_CPP_SOURCE_DIR}/Metal/Metal.hpp")
		message(FATAL_ERROR
			"Pinned Metal-cpp source is missing. Prepare it with ./DevTool dependency prepare --libs metal-cpp.")
	endif()
	target_include_directories(MetalRHI PRIVATE "${METAL_CPP_SOURCE_DIR}")
	target_sources(MetalRHI PRIVATE Private/MetalCppDevice.cpp Private/MetalDynamicRHI.mm)
	set_source_files_properties(Private/MetalDynamicRHI.mm PROPERTIES
		SKIP_PRECOMPILE_HEADERS ON
		COMPILE_FLAGS "-fobjc-arc")
	find_library(METAL_FRAMEWORK Metal REQUIRED)
	find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
	find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
	target_link_libraries(MetalRHI PRIVATE
		${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
endif()
