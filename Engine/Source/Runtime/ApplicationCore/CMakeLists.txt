add_durin_module(ApplicationCore)

target_link_libraries(ApplicationCore PRIVATE
    Vulkan::Vulkan
    glfw
)

if(WIN32)
	target_link_libraries(ApplicationCore PRIVATE dwmapi ole32 shell32)
elseif(APPLE)
	enable_language(OBJCXX)
	target_sources(ApplicationCore PRIVATE
		Private/MacOS/MacOSCustomTitleBarBridge.mm
	)
	set_source_files_properties(
		Private/MacOS/MacOSCustomTitleBarBridge.mm
		PROPERTIES SKIP_PRECOMPILE_HEADERS ON
	)
	find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
	find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
	target_link_libraries(ApplicationCore PRIVATE
		${APPKIT_FRAMEWORK}
		${QUARTZCORE_FRAMEWORK}
	)
endif()
