add_durin_module(RenderCore)

target_link_libraries(RenderCore PRIVATE
    Slang_Imported
)

durin_target_enable_delay_load(RenderCore "slang.dll")
durin_target_deploy_runtime_files(RenderCore Slang_Imported)
