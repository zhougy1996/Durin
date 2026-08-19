add_durin_module(AssetForge)

target_link_libraries(AssetForge PRIVATE assimp::assimp)

durin_target_enable_delay_load(AssetForge "$<TARGET_FILE_NAME:assimp::assimp>")
durin_target_deploy_runtime_files(AssetForge assimp::assimp)
