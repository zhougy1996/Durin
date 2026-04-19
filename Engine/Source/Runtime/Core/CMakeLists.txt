
doge_add_module(Core)

target_sources(Core PRIVATE
    ${DOGE_PROJECT_DIR}/Extras/VisualStudioDebugging/Doge.natvis
)

target_include_directories(Core PRIVATE
    ${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/xxhash
)

target_link_libraries(Core PRIVATE
    spdlog::spdlog
)

get_target_property(_t ryml::ryml TYPE)
message(STATUS "ryml target exists: ${_t}")

target_link_libraries(Core PUBLIC
    glm::glm
    glaze::glaze
    ryml::ryml
)