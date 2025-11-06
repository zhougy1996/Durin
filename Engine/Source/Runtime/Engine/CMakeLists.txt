project(Engine)

doge_print_project_build_info()

set(DHT_INPUT_HEADERS
	"Public/Engine/Actor.h"
	"Public/Actors/StaticMeshActor.h"
	"Public/Components/ActorComponent.h"
	"Public/Components/PrimitiveComponent.h"
	"Public/Components/SceneComponent.h"
	"Public/Components/MeshComponent.h"
	"Public/Components/StaticMeshComponent.h"
)

doge_target_set_dht_headers(${PROJECT_NAME} "${DHT_INPUT_HEADERS}" DHT_GENERATED_FILES)

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


