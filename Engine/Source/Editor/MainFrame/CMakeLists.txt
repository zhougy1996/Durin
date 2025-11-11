project(MainFrame)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(MainFrame SHARED ${SRCS})
doge_setup_shared_library(MainFrame)

target_include_directories(${PROJECT_NAME} PRIVATE
	${DOGE_PROJECT_SOURCE_DIR}/Editor/LevelEditor/Public
)

target_link_libraries(MainFrame PRIVATE
    Core
    RHI
    Engine
    ApplicationCore
    MonaCore
    Mona
    DogeEd
)


