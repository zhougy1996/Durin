project(DogeEd)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(DogeEd SHARED ${SRCS})
doge_setup_shared_library(DogeEd)

target_include_directories(DogeEd PRIVATE
    ${DOGE_PROJECT_SOURCE_DIR}/Editor/MainFrame/Public
)

target_link_libraries(DogeEd PRIVATE
    Core
    RHI
    Engine
    ApplicationCore
    MonaCore
    Mona
)


