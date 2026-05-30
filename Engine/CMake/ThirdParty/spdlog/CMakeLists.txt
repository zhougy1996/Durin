cmake_minimum_required(VERSION 3.24)

include(${CMAKE_CURRENT_LIST_DIR}/../DurinThirdParty.cmake)

function(durin_spdlog_configure_imported_target)
	durin_third_party_get_install_dir(spdlog_install_dir "spdlog")
	set(spdlog_include_dir "${spdlog_install_dir}/include")
	set(spdlog_lib_dir "${spdlog_install_dir}/lib")

	durin_third_party_find_single_file(spdlog_library "${spdlog_lib_dir}"
		"spdlog.lib"
		"spdlogd.lib"
		"libspdlog.a"
	)

	if(NOT EXISTS "${spdlog_include_dir}/spdlog/spdlog.h" OR NOT spdlog_library)
		durin_third_party_get_arch(durin_arch)
		message(FATAL_ERROR
			"Installed spdlog package was not found for ${durin_arch} ${CMAKE_BUILD_TYPE}.\n"
			"Expected install root: ${spdlog_install_dir}\n"
			"Prepare spdlog first, for example:\n"
			"  Engine/Scripts/Bootstrap/Setup_spdlog.bat ${CMAKE_BUILD_TYPE}\n"
			"or use an equivalent cmake -S/-B/-DCMAKE_INSTALL_PREFIX flow."
		)
	endif()

	add_library(spdlog STATIC IMPORTED GLOBAL)
	set_target_properties(spdlog PROPERTIES
		IMPORTED_LOCATION "${spdlog_library}"
		INTERFACE_INCLUDE_DIRECTORIES "${spdlog_include_dir}"
	)

	add_library(spdlog::spdlog ALIAS spdlog)
endfunction()

function(durin_spdlog_configure_source_build)
	project(DurinThirdPartySpdlog LANGUAGES CXX)

	get_filename_component(durin_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
	set(spdlog_source_dir "${durin_root}/Engine/Source/ThirdParty/spdlog")

	if(NOT EXISTS "${spdlog_source_dir}/CMakeLists.txt")
		message(FATAL_ERROR
			"spdlog source was not found at ${spdlog_source_dir}.\n"
			"Run Engine/Scripts/Bootstrap/Setup_spdlog.bat first to clone the source."
		)
	endif()

	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
	set(SPDLOG_INSTALL ON CACHE BOOL "" FORCE)
	set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(SPDLOG_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
	set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)

	add_subdirectory("${spdlog_source_dir}" "${CMAKE_BINARY_DIR}/spdlog")
endfunction()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	durin_spdlog_configure_source_build()
	return()
endif()

durin_spdlog_configure_imported_target()
