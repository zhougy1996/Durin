add_durin_module(AssetCore)

target_include_directories(AssetCore PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Source
)

target_link_libraries(AssetCore PRIVATE
    assimp::assimp
)

durin_target_enable_delay_load(AssetCore "$<TARGET_FILE_NAME:assimp::assimp>")
durin_target_deploy_runtime_files(AssetCore assimp::assimp)
