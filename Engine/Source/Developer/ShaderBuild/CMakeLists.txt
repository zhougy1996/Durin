add_durin_module(ShaderBuild)

target_link_libraries(ShaderBuild PRIVATE
    Slang_Imported
)

if(APPLE)
    target_link_libraries(ShaderBuild PRIVATE spirv-cross-c)
    set(shader_spirv_cross_manifest "${CMAKE_SOURCE_DIR}/Tools/DurinDevTool/durin_dev_tool/bootstrap/thirdparty/spirv-cross.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${shader_spirv_cross_manifest}")
    file(READ "${shader_spirv_cross_manifest}" shader_spirv_cross_json)
    string(JSON shader_spirv_cross_revision GET "${shader_spirv_cross_json}" source commit)
    target_compile_definitions(ShaderBuild PRIVATE DURIN_SHADER_SPIRV_CROSS_REVISION="${shader_spirv_cross_revision}")
endif()

durin_target_enable_delay_load(ShaderBuild "slang.dll")
durin_target_deploy_runtime_files(ShaderBuild Slang_Imported)
