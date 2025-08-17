cmake_minimum_required(VERSION 3.24)
project(Doge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
add_definitions(-DUNICODE -D_UNICODE)

if(MSVC)
    add_compile_options(/MP)
    add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/utf-8>)
endif()

set(DOGE_ARCH "x64")
message("Arch: ${DOGE_ARCH}")
message("CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")
message("")
message("Custom Macros(DOGE):")
set(DOGE_ROOT_DIR "${PROJECT_SOURCE_DIR}")
message("DOGE_ROOT_DIR: ${DOGE_ROOT_DIR}")
set(DOGE_ENGINE_ROOT_DIR "${DOGE_ROOT_DIR}/Engine")
message("DOGE_ENGINE_ROOT_DIR: ${DOGE_ENGINE_ROOT_DIR}")
set(DOGE_SOURCE_DIR "${DOGE_ENGINE_ROOT_DIR}/Source")
message("DOGE_SOURCE_DIR: ${DOGE_SOURCE_DIR}")
set(DOGE_BINARY_DIR "${DOGE_ENGINE_ROOT_DIR}/Binaries")
message("DOGE_BINARY_DIR: ${DOGE_BINARY_DIR}")
set(DOGE_INTERMEDIATE_DIR "${DOGE_ENGINE_ROOT_DIR}/Intermediate")
message("DOGE_INTERMEDIATE_DIR: ${DOGE_INTERMEDIATE_DIR}")
set(DOGE_THIRDPARTY_DIR "${DOGE_SOURCE_DIR}/ThirdParty")
message("DOGE_THIRDPARTY_DIR: ${DOGE_THIRDPARTY_DIR}")

list(APPEND CMAKE_PREFIX_PATH "${DOGE_SOURCE_DIR}/ThirdParty")


function(doge_set_module_output target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${DOGE_BINARY_DIR}/Doge/${DOGE_ARCH}/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${DOGE_BINARY_DIR}/${target}/${DOGE_ARCH}/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${DOGE_BINARY_DIR}/${target}/${DOGE_ARCH}/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${DOGE_BINARY_DIR}/${target}/${DOGE_ARCH}/$<CONFIG>"
    )
endfunction()

add_subdirectory(Engine/Source)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DogeLauncher)
