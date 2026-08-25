add_durin_module(AssetForgeBuiltins)

target_link_libraries(AssetForgeBuiltins PRIVATE assimp::assimp)

durin_target_enable_delay_load(AssetForgeBuiltins "$<TARGET_FILE_NAME:assimp::assimp>")
durin_target_deploy_runtime_files(AssetForgeBuiltins assimp::assimp)
