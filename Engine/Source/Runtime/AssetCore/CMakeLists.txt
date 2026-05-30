durin_add_module(AssetCore)

target_link_libraries(AssetCore PRIVATE
    assimp::assimp
)

durin_enable_delay_load(AssetCore "$<TARGET_FILE_NAME:assimp::assimp>")
durin_deploy_runtime_files_for_target(AssetCore assimp::assimp)
