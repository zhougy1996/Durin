add_durin_module(AssetForge)

# These persistence and execution implementations reuse private helper names
# also owned by other AssetForge translation units.
set_source_files_properties(
    "Private/ImportExecution.cpp"
    "Private/ImportRecord.cpp"
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE
)
