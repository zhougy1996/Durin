doge_add_module(RenderCore)

include(${DOGE_PROJECT_DIR}/CMake/ThirdParty/slang.cmake)

target_link_libraries(RenderCore PRIVATE Slang::slang)
add_copy_slang_runtime_command(RenderCore)