
doge_add_module(Core)

target_sources(Core PRIVATE
    ${DOGE_PROJECT_DIR}/Extras/VisualStudioDebugging/Doge.natvis
)

target_include_directories(Core PRIVATE
    ${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/xxhash
)

include(${DOGE_PROJECT_CMAKE_DIR}/ThirdParty/spdlog.cmake)
include(${DOGE_PROJECT_CMAKE_DIR}/ThirdParty/glm.cmake)
include(${DOGE_PROJECT_CMAKE_DIR}/ThirdParty/glaze.cmake)
target_link_libraries(Core PRIVATE
    Doge_spdlog
)
target_link_libraries(Core PUBLIC
    Doge_glm
    glaze::glaze
)