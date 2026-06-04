cmake_minimum_required(VERSION 3.24)

project(Durin LANGUAGES CXX)
include(CTest)

set(DURIN_CMAKE_DIR "${PROJECT_SOURCE_DIR}/CMake")
include("${DURIN_CMAKE_DIR}/DurinWorkspaceSetup.cmake")

add_subdirectory(Engine)
add_subdirectory(SandBox)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DurinLauncher)
