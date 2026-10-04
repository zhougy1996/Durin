add_durin_module(MonaImGui)

if(APPLE)
	enable_language(OBJCXX)
	target_sources(MonaImGui PRIVATE Private/Backend/MacOSModifierRecovery.mm)
	set_source_files_properties(Private/Backend/MacOSModifierRecovery.mm
		PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
	find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
	target_link_libraries(MonaImGui PRIVATE ${APPKIT_FRAMEWORK})
endif()

target_include_directories(MonaImGui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Public/ThirdParty/ImGui
)

target_compile_definitions(MonaImGui PUBLIC
    IMGUI_USER_CONFIG=\"ImGui/MonaImGuiConfig.h\"
)
