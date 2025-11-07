project(DogeProject_Engine)

set(MODULE_SEARCH_PATHS
    "${DOGE_SOURCE_DIR}/Runtime"
    "${DOGE_SOURCE_DIR}/Editor"
)

function(build_module_index project_name module_search_paths)
    add_custom_target(GenerateModuleIndex_${project_name} ALL
        COMMAND ${PYTHON_EXECUTABLE} ${DHT_BUILD_MODULE_INDEX_EXE} --project ${project_name} --project_dir ${PROJECT_SOURCE_DIR} --search_paths "${MODULE_SEARCH_PATHS}"
        COMMENT "Updating Module Index for ${project_name}"
        DEPENDS ${DHT_BUILD_MODULE_INDEX_EXE}
        VERBATIM
    )
endfunction()

build_module_index(Engine ${MODULE_SEARCH_PATHS})

add_subdirectory(Source)