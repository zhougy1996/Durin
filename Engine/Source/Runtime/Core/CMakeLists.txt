
add_durin_module(Core)

target_sources(Core PRIVATE
    ${DURIN_PROJECT_ROOT_DIR}/Extras/VisualStudioDebugging/Durin.natvis
    Private/Threading/QueuedThreadPool.cpp
    Private/Threading/Task.cpp
)

target_include_directories(Core PRIVATE
    ${DURIN_PROJECT_SOURCE_DIR}/ThirdParty/xxhash
)

target_link_libraries(Core PRIVATE
    spdlog::spdlog
    yyjson::yyjson
)

target_link_libraries(Core PUBLIC
    glm::glm
    ryml::ryml
)
