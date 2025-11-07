project(Engine)

doge_print_project_build_info()


doge_target_set_dht_headers(${PROJECT_NAME} DHT_GENERATED_FILES)

doge_collect_and_organize_source_files(SRCS)

add_library(Engine SHARED
	${SRCS}
	${DHT_GENERATED_FILES}
)
doge_setup_shared_library(Engine)

target_link_libraries(Engine PRIVATE
    Core
    RHI
    RenderCore
    ApplicationCore
    MonaCore
    Mona
)


