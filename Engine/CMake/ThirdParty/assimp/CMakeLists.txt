cmake_minimum_required(VERSION 3.24)

function(durin_assimp_find_single_file out_var base_dir)
	foreach(candidate IN LISTS ARGN)
		if(EXISTS "${base_dir}/${candidate}")
			set(${out_var} "${base_dir}/${candidate}" PARENT_SCOPE)
			return()
		endif()
	endforeach()

	set(${out_var} "" PARENT_SCOPE)
endfunction()

function(durin_assimp_configure_imported_target)
	get_filename_component(durin_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

	if(MSVC)
		set(durin_arch "Win64")
	elseif(APPLE)
		set(durin_arch "MacOS")
	else()
		set(durin_arch "${CMAKE_SYSTEM_NAME}")
	endif()

	if(NOT CMAKE_BUILD_TYPE)
		message(FATAL_ERROR "Durin requires CMAKE_BUILD_TYPE to import the shared assimp installation.")
	endif()

	set(assimp_install_dir "${durin_root}/Build/ThirdParty/Install/${durin_arch}/${CMAKE_BUILD_TYPE}/assimp")
	set(assimp_include_dir "${assimp_install_dir}/include")
	set(assimp_lib_dir "${assimp_install_dir}/lib")
	set(assimp_bin_dir "${assimp_install_dir}/bin")

	durin_assimp_find_single_file(assimp_import_lib "${assimp_lib_dir}"
		"assimp-vc143-mt.lib"
		"assimp.lib"
	)
	durin_assimp_find_single_file(assimp_runtime_dll "${assimp_bin_dir}"
		"assimp-vc143-mt.dll"
		"assimp.dll"
	)

	if(NOT EXISTS "${assimp_include_dir}/assimp/Importer.hpp" OR NOT assimp_import_lib OR NOT assimp_runtime_dll)
		message(FATAL_ERROR
			"Shared assimp installation was not found for ${durin_arch} ${CMAKE_BUILD_TYPE}.\n"
			"Expected install root: ${assimp_install_dir}\n"
			"Prepare assimp first, for example:\n"
			"  cmake --preset ${durin_arch}-${CMAKE_BUILD_TYPE}-assimp\n"
			"  cmake --build --preset ${durin_arch}-${CMAKE_BUILD_TYPE}-assimp\n"
			"from ${CMAKE_CURRENT_LIST_DIR}, or use an equivalent -S/-B/-DCMAKE_INSTALL_PREFIX command."
		)
	endif()

	add_library(assimp::assimp SHARED IMPORTED GLOBAL)
	set_target_properties(assimp::assimp PROPERTIES
		IMPORTED_LOCATION "${assimp_runtime_dll}"
		IMPORTED_IMPLIB "${assimp_import_lib}"
		INTERFACE_INCLUDE_DIRECTORIES "${assimp_include_dir}"
	)
endfunction()

function(durin_assimp_configure_source_build)
	project(DurinThirdPartyAssimp LANGUAGES C CXX)

	get_filename_component(durin_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
	set(assimp_source_dir "${durin_root}/Engine/Source/ThirdParty/assimp")

	if(NOT EXISTS "${assimp_source_dir}/CMakeLists.txt")
		message(FATAL_ERROR
			"Assimp source was not found at ${assimp_source_dir}.\n"
			"Run Engine/Scripts/Bootstrap/Setup_assimp.bat first to clone the source."
		)
	endif()

	set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
	set(ASSIMP_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
	set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "")
	set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "")
	set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "")
	set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "")
	set(ASSIMP_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/assimp/lib" CACHE STRING "" FORCE)
	set(ASSIMP_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/assimp/bin" CACHE STRING "" FORCE)
	set(ASSIMP_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/assimp/bin" CACHE STRING "" FORCE)
	set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "")
	set(ASSIMP_INSTALL ON CACHE BOOL "" FORCE)
	set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "")
	set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "Disable Assimp samples" FORCE)
	set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "")
	set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "")
	set(ASSIMP_INSTALL_PDB OFF CACHE BOOL "" FORCE)

	add_subdirectory("${assimp_source_dir}" "${CMAKE_BINARY_DIR}/assimp")
endfunction()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	durin_assimp_configure_source_build()
	return()
endif()

durin_assimp_configure_imported_target()
