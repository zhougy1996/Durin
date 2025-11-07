cmake_minimum_required(VERSION 3.24)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(Doge LANGUAGES CXX)

set(DOGE_ROOT_DIR "${PROJECT_SOURCE_DIR}")
set(DOGE_CMAKE_INCLUDE_DIR "${DOGE_ROOT_DIR}/Engine/Configs/CMake")

include(${DOGE_CMAKE_INCLUDE_DIR}/DogeCommon.cmake)
include(${DOGE_CMAKE_INCLUDE_DIR}/DogeModules.cmake)

add_subdirectory(Engine)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DogeLauncher)
