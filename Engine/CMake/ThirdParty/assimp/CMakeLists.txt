cmake_minimum_required(VERSION 3.24)

function(durin_assimp_configure_imported_target)
	durin_third_party_get_install_dir(assimp_install_dir "assimp")
	set(assimp_include_dir "${assimp_install_dir}/include")
	set(assimp_lib_dir "${assimp_install_dir}/lib")
	set(assimp_bin_dir "${assimp_install_dir}/bin")

	durin_third_party_find_single_file(assimp_import_lib "${assimp_lib_dir}"
		"assimp-vc143-mt.lib"
		"assimp.lib"
		"libassimp.dylib"
	)
	durin_third_party_find_single_file(assimp_runtime_dll "${assimp_bin_dir}"
		"assimp-vc143-mt.dll"
		"assimp.dll"
	)

	if(APPLE)
		set(assimp_runtime "${assimp_import_lib}")
	else()
		set(assimp_runtime "${assimp_runtime_dll}")
	endif()

	if(NOT EXISTS "${assimp_include_dir}/assimp/Importer.hpp" OR NOT assimp_import_lib OR NOT assimp_runtime)
		durin_third_party_get_arch(durin_arch)
		message(FATAL_ERROR
			"Installed assimp package was not found for ${durin_arch} ${CMAKE_BUILD_TYPE}.\n"
			"Expected install root: ${assimp_install_dir}\n"
			"Prepare assimp first, for example:\n"
			"  DevTool.bat dependency prepare --libs assimp --config ${CMAKE_BUILD_TYPE}\n"
			"or use an equivalent cmake -S/-B/-DCMAKE_INSTALL_PREFIX flow."
		)
	endif()

	add_library(assimp::assimp SHARED IMPORTED GLOBAL)
	set_target_properties(assimp::assimp PROPERTIES
		IMPORTED_LOCATION "${assimp_runtime}"
		INTERFACE_INCLUDE_DIRECTORIES "${assimp_include_dir}"
		DURIN_RUNTIME_DEPLOY_FILES "${assimp_runtime}"
	)
	if(WIN32)
		set_target_properties(assimp::assimp PROPERTIES
			IMPORTED_IMPLIB "${assimp_import_lib}"
		)
	endif()
endfunction()

function(durin_assimp_configure_source_build)
	project(DurinThirdPartyAssimp LANGUAGES C CXX)

	get_filename_component(durin_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
	set(assimp_source_dir "${durin_root}/Engine/External/Source/assimp")

	if(NOT EXISTS "${assimp_source_dir}/CMakeLists.txt")
		message(FATAL_ERROR
			"Assimp source was not found at ${assimp_source_dir}.\n"
			"Run DevTool.bat dependency prepare --libs assimp first."
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
	if(WIN32)
		set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
	else()
		# Assimp's bundled zlib 1.2.13 treats modern Darwin as both classic
		# MacOS and Apple, which conflicts with current Xcode SDK headers.
		# Use the platform zlib on POSIX hosts, matching Assimp's own default.
		set(ASSIMP_BUILD_ZLIB OFF CACHE BOOL "" FORCE)
	endif()
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
