
add_durin_module(Core)

set(_durin_version_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated")
file(MAKE_DIRECTORY "${_durin_version_generated_dir}/Misc")
configure_file(
	"${DURIN_CMAKE_DIR}/Templates/DurinVersion.gen.h.in"
	"${_durin_version_generated_dir}/Misc/DurinVersion.gen.h"
	@ONLY
)
target_include_directories(Core PUBLIC "${_durin_version_generated_dir}")

target_sources(Core PRIVATE
    ${DURIN_PROJECT_ROOT_DIR}/Extras/VisualStudioDebugging/Durin.natvis
	Private/Console/ConsoleCommand.cpp
	Private/Image/ImageDecoder.cpp
    Private/Threading/QueuedThreadPool.cpp
    Private/Threading/Task.cpp
)

target_include_directories(Core PRIVATE
	${DURIN_PROJECT_SOURCE_DIR}
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
