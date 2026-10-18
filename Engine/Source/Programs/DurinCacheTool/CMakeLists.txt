# Offline DDC maintenance needs neither an engine loop nor asset/editor modules.
add_executable(DurinCacheTool "Private/CacheToolMain.cpp")
durin_target_enable_windows_long_paths(DurinCacheTool)
durin_target_apply_common_definitions(DurinCacheTool "DurinCacheTool")
target_link_libraries(DurinCacheTool PRIVATE Core DerivedDataCache)
get_target_property(_durin_cache_tool_runtime_dir DurinLauncher RUNTIME_OUTPUT_DIRECTORY)
set_target_properties(DurinCacheTool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_durin_cache_tool_runtime_dir}"
    FOLDER "Engine/Programs")
