cmake_minimum_required(VERSION 3.24)



project(DurinEngine LANGUAGES CXX)

set(DURIN_CMAKE_DIR "${PROJECT_SOURCE_DIR}/CMake")

add_subdirectory(Engine)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DurinLauncher)
