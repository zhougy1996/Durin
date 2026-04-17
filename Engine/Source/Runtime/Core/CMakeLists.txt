doge_add_module(Core)

target_sources(Core PRIVATE
    ${DOGE_PROJECT_DIR}/Extras/VisualStudioDebugging/Doge.natvis
)

target_include_directories(Core PRIVATE
    ${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/xxhash
)

target_include_directories(Core PUBLIC
    ${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/glm
)

include(${DOGE_PROJECT_CMAKE_DIR}/ThirdParty/spdlog.cmake)
target_link_libraries(Core PRIVATE
    Doge_spdlog
)