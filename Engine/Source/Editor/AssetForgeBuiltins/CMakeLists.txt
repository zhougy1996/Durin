add_durin_module(AssetForgeBuiltins)

# Provider implementations intentionally share anonymous-namespace schemas and
# reconciliation helpers across translation units.
set_target_properties(AssetForgeBuiltins PROPERTIES UNITY_BUILD OFF)

target_link_libraries(AssetForgeBuiltins PRIVATE assimp::assimp)

durin_target_enable_delay_load(AssetForgeBuiltins "$<TARGET_FILE_NAME:assimp::assimp>")
durin_target_deploy_runtime_files(AssetForgeBuiltins assimp::assimp)
