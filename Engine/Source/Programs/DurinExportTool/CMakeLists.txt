add_executable(DurinExportTool Private/ExportToolMain.cpp)
durin_target_enable_windows_long_paths(DurinExportTool)
durin_target_apply_common_definitions(DurinExportTool "DurinExportTool")
target_link_libraries(DurinExportTool PRIVATE GameExport)
get_target_property(_export_runtime_dir DurinLauncher RUNTIME_OUTPUT_DIRECTORY)
set_target_properties(DurinExportTool PROPERTIES OUTPUT_NAME "DurinExportTool"
    RUNTIME_OUTPUT_DIRECTORY "${_export_runtime_dir}" FOLDER "Engine/Programs")
