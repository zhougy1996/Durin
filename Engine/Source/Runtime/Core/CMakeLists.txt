doge_add_module(Core)

target_sources(Core PRIVATE
    ${DOGE_ENGINE_DIR}/Extras/VisualStudioDebugging/Doge.natvis
)

target_include_directories(Core PRIVATE
    ${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/spdlog/include
    ${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/xxhash
)

target_include_directories(Core PUBLIC
    ${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/glm
    ${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/rapidjson/include
)

target_link_libraries(Core PRIVATE 
    #spdlog::spdlog_header_only
    spdlog::spdlog
)