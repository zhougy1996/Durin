cmake_minimum_required(VERSION 3.24)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(DogeHeaderTool LANGUAGES CXX)

get_filename_component(DOGE_ROOT_DIR "${PROJECT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(DOGE_CMAKE_INCLUDE_DIR "${DOGE_ROOT_DIR}/Engine/Configs/CMake")

include(${DOGE_CMAKE_INCLUDE_DIR}/DogeCommon.cmake)
include(${DOGE_CMAKE_INCLUDE_DIR}/DogeModules.cmake)

file(GLOB_RECURSE SRCS
	"Private/*.h"
	"Private/*.cpp"
)

add_executable(DogeHeaderTool
	${SRCS}
)

target_precompile_headers(${PROJECT_NAME} PRIVATE
	"$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Private/PCH.${PROJECT_NAME}.h>"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${DOGE_THIRDPARTY_DIR}/clang/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
	${DOGE_THIRDPARTY_DIR}/clang/lib/libclang.lib
)

