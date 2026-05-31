durin_add_module(MonaImGuiBackend)

target_include_directories(MonaImGuiBackend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Public/ThirdParty/ImGui
)

target_compile_definitions(MonaImGuiBackend PUBLIC
    IMGUI_USER_CONFIG=\"DurinImGuiConfig.h\"
)
