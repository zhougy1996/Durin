cmake_minimum_required(VERSION 3.24)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(DogeEngine LANGUAGES CXX)

set(DOGE_CMAKE_DIR "${PROJECT_SOURCE_DIR}/CMake")

include(${DOGE_CMAKE_DIR}/Common.cmake)
include(${DOGE_CMAKE_DIR}/Modules.cmake)

add_subdirectory(Engine)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DogeLauncher)
