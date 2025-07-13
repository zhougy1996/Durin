project(LevelEditor)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(LevelEditor SHARED ${SRCS})
doge_setup_shared_library(LevelEditor)

target_link_libraries(LevelEditor PRIVATE
    Core
    RHI
    Engine
    ApplicationCore
    MonaCore
    Mona
    DogeEd
)


