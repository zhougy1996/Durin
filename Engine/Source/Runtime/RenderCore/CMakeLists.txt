doge_add_module(RenderCore)

target_link_options(RenderCore PRIVATE
    "/DELAYLOAD:slang.dll"
)

target_link_libraries(RenderCore PRIVATE Slang_Interface)
target_link_libraries(RenderCore PRIVATE delayimp)

add_copy_slang_runtime_command(RenderCore)