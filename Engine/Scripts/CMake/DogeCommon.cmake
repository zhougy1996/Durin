include_guard(GLOBAL)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
add_definitions(-DUNICODE -D_UNICODE)

if(MSVC)
	add_compile_options(/MP)
	add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/utf-8>)
endif()

if(CMAKE_GENERATOR STREQUAL "Ninja")
	message("CMake Generator: Ninja")
endif()

if(MSVC)
	set(DOGE_ARCH "Win64")
elseif(APPLE)
	set(DOGE_ARCH "MacOS")
	set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()

message("Arch: ${DOGE_ARCH}")
message("CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")

get_filename_component(DOGE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
message(DOGE_DIR: ${DOGE_DIR})

find_package(Vulkan)
set(Python_ROOT_DIR "${DOGE_DIR}/.venv")
find_package(Python REQUIRED COMPONENTS Interpreter Development)

# Define paths for the Doge engine project structure
set(DOGE_ENGINE_DIR "${DOGE_DIR}/Engine")
set(DOGE_ENGINE_CONFIG_DIR "${DOGE_ENGINE_DIR}/Configs")
set(DOGE_ENGINE_SCRIPT_DIR "${DOGE_ENGINE_DIR}/Scripts")
set(DOGE_ENGINE_SOURCE_DIR "${DOGE_ENGINE_DIR}/Source")
set(DOGE_ENGINE_BINARY_DIR "${DOGE_ENGINE_DIR}/Binaries")
set(DOGE_ENGINE_INTERMEDIATE_DIR "${DOGE_ENGINE_DIR}/Intermediate")
set(DOGE_ENGINE_THIRDPARTY_DIR "${DOGE_ENGINE_SOURCE_DIR}/ThirdParty")

list(APPEND CMAKE_PREFIX_PATH "${DOGE_ENGINE_SOURCE_DIR}/ThirdParty")


