project(Mona)

doge_print_project_build_info()

doge_collect_and_organize_source_files(SRCS)

add_library(Mona SHARED ${SRCS})
doge_setup_shared_library(Mona)

target_include_directories(Mona PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Public/ThirdParty/ImGui
)

target_link_libraries(Mona PRIVATE
    Core
    RHI
    MonaCore
    ApplicationCore
)


