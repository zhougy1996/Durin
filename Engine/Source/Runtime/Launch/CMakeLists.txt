project(Launch)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(Launch SHARED ${SRCS})
doge_setup_shared_library(Launch)

target_link_libraries(Launch PRIVATE
    Core
    RHI
    Engine
    ApplicationCore
    MonaCore
    Mona
    DogeEd
)


