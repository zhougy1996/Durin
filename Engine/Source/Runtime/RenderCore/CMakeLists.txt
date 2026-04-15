doge_add_module(RenderCore)

include(${DOGE_PROJECT_DIR}/CMake/ThirdParty/slang.cmake)

target_link_libraries(RenderCore PRIVATE Slang::slang)