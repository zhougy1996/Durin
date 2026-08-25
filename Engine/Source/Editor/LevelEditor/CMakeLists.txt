add_durin_module(LevelEditor)

# These panels and picking services share private helper names with adjacent
# implementations in the module.
set_source_files_properties(
    "Private/Assets/TextureImportDialog.cpp"
    "Private/Authoring/TerrainLevelAuthoring.cpp"
    "Private/Panels/WorldOutlinerPanel.cpp"
    "Private/Viewport/ViewportPickingService.cpp"
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE
)

target_include_directories(LevelEditor PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Source
)
