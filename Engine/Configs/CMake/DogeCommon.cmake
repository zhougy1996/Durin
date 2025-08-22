# Define DOGE_ROOT_DIR before including Doge.cmake

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
add_definitions(-DUNICODE -D_UNICODE)

if(MSVC)
	add_compile_options(/MP)
	add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/utf-8>)
endif()

if(CMAKE_GENERATOR STREQUAL "Ninja")
	message("CMake Generator: Ninja")
endif()

set(DOGE_ARCH "x64")
message("Arch: ${DOGE_ARCH}")
message("CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")

if(NOT DEFINED DOGE_ROOT_DIR)
	message(FATAL_ERROR "DOGE_ROOT_DIR is not defined. Please set it before including Doge.cmake.")
endif()

# Define paths for the Doge engine project structure
set(DOGE_ENGINE_ROOT_DIR "${DOGE_ROOT_DIR}/Engine")
set(DOGE_CONFIG_DIR "${DOGE_ENGINE_ROOT_DIR}/Configs")
set(DOGE_SOURCE_DIR "${DOGE_ENGINE_ROOT_DIR}/Source")
set(DOGE_BINARY_DIR "${DOGE_ENGINE_ROOT_DIR}/Binaries")
set(DOGE_INTERMEDIATE_DIR "${DOGE_ENGINE_ROOT_DIR}/Intermediate")
set(DOGE_THIRDPARTY_DIR "${DOGE_SOURCE_DIR}/ThirdParty")

list(APPEND CMAKE_PREFIX_PATH "${DOGE_SOURCE_DIR}/ThirdParty")

message("")
message("Project Structure(Doge):")
message("DOGE_ROOT_DIR: ${DOGE_ROOT_DIR}")
message("DOGE_ENGINE_ROOT_DIR: ${DOGE_ENGINE_ROOT_DIR}")
message("DOGE_CONFIG_DIR: ${DOGE_CONFIG_DIR}")
message("DOGE_SOURCE_DIR: ${DOGE_SOURCE_DIR}")
message("DOGE_BINARY_DIR: ${DOGE_BINARY_DIR}")
message("DOGE_INTERMEDIATE_DIR: ${DOGE_INTERMEDIATE_DIR}")
message("DOGE_THIRDPARTY_DIR: ${DOGE_THIRDPARTY_DIR}")


