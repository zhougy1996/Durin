durin_add_module(AssetCore)

target_link_options(AssetCore PRIVATE
    "/DELAYLOAD:$<TARGET_FILE_NAME:assimp::assimp>"
)
target_link_libraries(AssetCore PRIVATE
    assimp::assimp
    delayimp
)

durin_copy_external_target_binary(AssetCore assimp::assimp)