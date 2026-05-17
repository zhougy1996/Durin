
durin_add_module(Core)

target_sources(Core PRIVATE
    ${DURIN_PROJECT_DIR}/Extras/VisualStudioDebugging/Durin.natvis
)

target_include_directories(Core PRIVATE
    ${DURIN_PROJECT_SOURCE_DIR}/ThirdParty/xxhash
)

target_link_libraries(Core PRIVATE
    spdlog::spdlog
)

target_link_libraries(Core PUBLIC
    glm::glm
    ryml::ryml
)