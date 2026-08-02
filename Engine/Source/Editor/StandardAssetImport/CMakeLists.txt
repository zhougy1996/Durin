add_durin_module(StandardAssetImport)

target_link_libraries(StandardAssetImport PRIVATE assimp::assimp)

durin_target_enable_delay_load(StandardAssetImport "$<TARGET_FILE_NAME:assimp::assimp>")
durin_target_deploy_runtime_files(StandardAssetImport assimp::assimp)
