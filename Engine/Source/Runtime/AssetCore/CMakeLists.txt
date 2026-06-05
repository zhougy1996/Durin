add_durin_module(AssetCore)

target_link_libraries(AssetCore PRIVATE
    assimp::assimp
)

durin_target_enable_delay_load(AssetCore "$<TARGET_FILE_NAME:assimp::assimp>")
durin_target_deploy_runtime_files(AssetCore assimp::assimp)
