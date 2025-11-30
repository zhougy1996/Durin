set(MODULE_LIST
    ApplicationCore
    RHI
    VulkanRHI
    RenderCore
    MonaCore
    Mona
    Renderer
    Launch
)

foreach(module IN LISTS MODULE_LIST)
    add_subdirectory(${module})
    set_target_properties(${module} PROPERTIES FOLDER "Engine/Runtime")
endforeach()
