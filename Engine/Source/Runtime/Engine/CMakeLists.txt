project(Engine)

doge_print_project_build_info()

set(DHT_INPUT_HEADERS
	"${CMAKE_CURRENT_SOURCE_DIR}/Public/Engine/Actor.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/Public/Actors/StaticMeshActor.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/Public/Components/ActorComponent.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/Public/Components/PrimitiveComponent.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/Public/Components/SceneComponent.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/Public/Components/MeshComponent.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/Public/Components/StaticMeshComponent.h"
)
doge_module_add_dht_input_headers(${PROJECT_NAME} "${DHT_INPUT_HEADERS}" DHT_GENERATED_FILES)

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


