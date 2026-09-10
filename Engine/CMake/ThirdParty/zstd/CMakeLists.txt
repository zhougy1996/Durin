cmake_minimum_required(VERSION 3.24)

get_filename_component(durin_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(zstd_source_dir "${durin_root}/Engine/External/Source/zstd")
foreach(required_file LICENSE COPYING lib/zstd.h build/cmake/CMakeLists.txt)
	if(NOT EXISTS "${zstd_source_dir}/${required_file}")
		message(FATAL_ERROR "Zstd source is incomplete. Run DevTool.bat dependency prepare --libs zstd first.")
	endif()
endforeach()

set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory("${zstd_source_dir}/build/cmake" "${CMAKE_CURRENT_BINARY_DIR}/upstream" EXCLUDE_FROM_ALL)
add_library(zstd::zstd ALIAS libzstd_static)
set_target_properties(libzstd_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Retain both upstream license alternatives with redistributed installations.
install(FILES "${zstd_source_dir}/LICENSE" "${zstd_source_dir}/COPYING"
	DESTINATION "share/licenses/zstd")
