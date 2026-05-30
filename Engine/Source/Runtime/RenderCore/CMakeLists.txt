durin_add_module(RenderCore)

target_link_libraries(RenderCore PRIVATE
    Slang_Imported
)

durin_enable_delay_load(RenderCore "slang.dll")
durin_deploy_runtime_files_for_target(RenderCore Slang_Imported)
