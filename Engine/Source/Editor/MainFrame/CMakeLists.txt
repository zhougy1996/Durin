project(MainFrame)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(MainFrame SHARED ${SRCS})
doge_setup_shared_library(MainFrame)

target_link_libraries(MainFrame PRIVATE
    Core
    RHI
    Engine
    ApplicationCore
    MonaCore
    Mona
    DogeEd
)


