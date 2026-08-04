# Read-only package compatibility host used by DurinDevTool. It deliberately
# links no Launch/editor executable and performs no engine-loop initialization.
if(DURIN_RUNTIME_VARIANT STREQUAL "DurinEditor")
    set(DURIN_WITH_EDITOR 1)
else()
    set(DURIN_WITH_EDITOR 0)
endif()
add_executable(DurinAssetAudit "Private/AssetAuditMain.cpp")
durin_target_enable_windows_long_paths(DurinAssetAudit)
durin_target_apply_common_definitions(DurinAssetAudit "DurinAssetAudit")
target_link_libraries(DurinAssetAudit PRIVATE AssetCore Engine)
if(DURIN_ENABLE_PCH)
    target_precompile_headers(DurinAssetAudit REUSE_FROM SharedPCH_Core)
else()
    get_property(_durin_core_pch GLOBAL PROPERTY "DURIN_SHARED_PCH_HEADER_SharedPCH_Core")
    if(_durin_core_pch)
        durin_target_force_include_pch(DurinAssetAudit "${_durin_core_pch}")
    endif()
endif()
get_target_property(_durin_audit_runtime_dir DurinLauncher RUNTIME_OUTPUT_DIRECTORY)
if(NOT _durin_audit_runtime_dir)
    message(FATAL_ERROR "DurinAssetAudit requires the configured runtime output directory.")
endif()
set_target_properties(DurinAssetAudit PROPERTIES
    OUTPUT_NAME "DurinAssetAudit"
    RUNTIME_OUTPUT_DIRECTORY "${_durin_audit_runtime_dir}"
    FOLDER "Engine/Programs"
)
