project(Engine)

doge_add_shared_library(Engine)

target_link_libraries(Engine PRIVATE
    Core
    RHI
    RenderCore
    ApplicationCore
    MonaCore
    Mona
)


