doge_add_module(RenderCore)

target_link_options(RenderCore PRIVATE
    "/DELAYLOAD:slang.dll"
)

include(${DOGE_PROJECT_CMAKE_DIR}/ThirdParty/slang.cmake)
target_link_libraries(RenderCore PRIVATE Doge_Slang)
target_link_libraries(RenderCore PRIVATE delayimp)

add_copy_slang_runtime_command(RenderCore)