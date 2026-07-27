add_durin_module(AssetImport)

target_include_directories(AssetImport PUBLIC
	"${DURIN_WORKSPACE_DIR}/Engine/Source/Runtime/Engine/Public"
	"${DURIN_PROJECT_INTERMEDIATE_BUILD_DIR}/Engine/DHT"
)

target_link_libraries(AssetImport PRIVATE assimp::assimp)

durin_target_enable_delay_load(AssetImport "$<TARGET_FILE_NAME:assimp::assimp>")
durin_target_deploy_runtime_files(AssetImport assimp::assimp)
