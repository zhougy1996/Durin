add_durin_module(ShaderBuild)

target_link_libraries(ShaderBuild PRIVATE
    Slang_Imported
)

if(APPLE)
    target_link_libraries(ShaderBuild PRIVATE spirv-cross-c)
endif()

durin_target_enable_delay_load(ShaderBuild "slang.dll")
durin_target_deploy_runtime_files(ShaderBuild Slang_Imported)
