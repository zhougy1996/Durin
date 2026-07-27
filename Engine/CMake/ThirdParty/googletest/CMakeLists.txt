cmake_minimum_required(VERSION 3.24)

get_filename_component(durin_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(googletest_source_dir "${durin_root}/Engine/External/Source/googletest")

if(NOT EXISTS "${googletest_source_dir}/CMakeLists.txt")
	message(FATAL_ERROR
		"googletest source was not found at ${googletest_source_dir}.\n"
		"Run Tools/DurinDevTool/DevTool.bat dependency prepare --libs googletest first."
	)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

add_subdirectory("${googletest_source_dir}" "${CMAKE_BINARY_DIR}/googletest")
