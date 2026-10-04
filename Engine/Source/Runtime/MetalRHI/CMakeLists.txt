add_durin_module(MetalRHI)

if(APPLE)
	set(METAL_CPP_SOURCE_DIR "${DURIN_PROJECT_ROOT_DIR}/External/Source/metal-cpp")
	if(NOT EXISTS "${METAL_CPP_SOURCE_DIR}/Metal/Metal.hpp")
		message(FATAL_ERROR
			"Pinned Metal-cpp source is missing. Prepare it with ./DevTool dependency prepare --libs metal-cpp.")
	endif()
	target_include_directories(MetalRHI PRIVATE
		"${METAL_CPP_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/Private/MacOS")
	target_compile_options(MetalRHI PRIVATE -fblocks)
	target_sources(MetalRHI PRIVATE
		Private/MetalCppDevice.cpp
		Private/MacOS/MetalResourceDescriptors.cpp
		Private/MacOS/MetalPipeline.cpp
		Private/MacOS/MetalSubmission.cpp
		Private/MacOS/MetalCommandContext.cpp
		Private/MacOS/MetalDynamicRHI.cpp)
	# Native headers and implementation macros compile independently of the Core PCH.
	set_source_files_properties(
		Private/MetalCppDevice.cpp Private/MacOS/MetalResourceDescriptors.cpp
		Private/MacOS/MetalPipeline.cpp Private/MacOS/MetalSubmission.cpp
		Private/MacOS/MetalCommandContext.cpp Private/MacOS/MetalDynamicRHI.cpp
		PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
	find_library(METAL_FRAMEWORK Metal REQUIRED)
	find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
	find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
	target_link_libraries(MetalRHI PRIVATE
		${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
endif()
