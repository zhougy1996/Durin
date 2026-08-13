# Capability-separated asset audit and migration host used by DurinDevTool. It
# deliberately links no Launch/editor executable and performs no engine loop.
if(DURIN_RUNTIME_VARIANT STREQUAL "DurinEditor")
    set(DURIN_WITH_EDITOR 1)
else()
    set(DURIN_WITH_EDITOR 0)
endif()
add_executable(DurinAssetTool "Private/AssetToolMain.cpp")
durin_target_enable_windows_long_paths(DurinAssetTool)
durin_target_apply_common_definitions(DurinAssetTool "DurinAssetTool")
target_link_libraries(DurinAssetTool PRIVATE AssetCore Engine AssetImportCore)
target_include_directories(DurinAssetTool PRIVATE
    "${DURIN_PROJECT_ROOT_DIR}/Source/Developer/AssetBuildCore/Public")
# Keep the optional Build DLL available for migrate operations without adding
# it to the package-only audit process import closure.
if(TARGET AssetBuildCore)
    add_dependencies(DurinAssetTool AssetBuildCore)
endif()
if(TARGET GeometryBuild)
    add_dependencies(DurinAssetTool GeometryBuild)
endif()
if(TARGET TextureBuild)
    add_dependencies(DurinAssetTool TextureBuild)
endif()
if(DURIN_ENABLE_PCH)
    target_precompile_headers(DurinAssetTool REUSE_FROM SharedPCH_Core)
else()
    get_property(_durin_core_pch GLOBAL PROPERTY "DURIN_SHARED_PCH_HEADER_SharedPCH_Core")
    if(_durin_core_pch)
        durin_target_force_include_pch(DurinAssetTool "${_durin_core_pch}")
    endif()
endif()
get_target_property(_durin_asset_tool_runtime_dir DurinLauncher RUNTIME_OUTPUT_DIRECTORY)
if(NOT _durin_asset_tool_runtime_dir)
    message(FATAL_ERROR "DurinAssetTool requires the configured runtime output directory.")
endif()
set_target_properties(DurinAssetTool PROPERTIES
    OUTPUT_NAME "DurinAssetTool"
    RUNTIME_OUTPUT_DIRECTORY "${_durin_asset_tool_runtime_dir}"
    FOLDER "Engine/Programs"
)
