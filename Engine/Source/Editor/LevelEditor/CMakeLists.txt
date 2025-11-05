project(LevelEditor)

doge_print_project_build_info()

set(DHT_INPUT_HEADERS
	"${CMAKE_CURRENT_SOURCE_DIR}/Private/TestDHT.h"
)


doge_module_add_dht_input_headers(${PROJECT_NAME} "${DHT_INPUT_HEADERS}" DHT_GENERATED_FILES)

doge_collect_and_organize_source_files(SRCS)

add_library(LevelEditor SHARED
	${DHT_GENERATED_FILES}
	${SRCS}
)

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


