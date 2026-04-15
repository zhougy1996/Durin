doge_add_module(RenderCore)

include(${DOGE_PROJECT_DIR}/CMake/ThirdParty/slang.cmake)

target_link_options(RenderCore PRIVATE
    "/DELAYLOAD:slang.dll"
)
target_link_libraries(RenderCore PRIVATE Slang::slang)
target_link_libraries(RenderCore PRIVATE delayimp)

add_copy_slang_runtime_command(RenderCore)