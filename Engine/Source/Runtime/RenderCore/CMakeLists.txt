add_durin_module(RenderCore)

# These files deliberately own independent anonymous-namespace Slang profile
# constants. Keep two outside the unity batch so those private identities do
# not collide with SlangShaderCompiler.cpp.
set_source_files_properties(
    "Private/Shader/ShaderCompileUtilities.cpp"
    "Private/Shader/SlangShaderDependencyResolver.cpp"
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE
)

target_link_libraries(RenderCore PRIVATE
    Slang_Imported
)

durin_target_enable_delay_load(RenderCore "slang.dll")
durin_target_deploy_runtime_files(RenderCore Slang_Imported)
