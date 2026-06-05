cmake_minimum_required(VERSION 3.24)

function(durin_rapidyaml_configure_imported_target)
	durin_third_party_get_install_dir(rapidyaml_install_dir "rapidyaml")
	set(rapidyaml_include_dir "${rapidyaml_install_dir}/include")
	set(rapidyaml_lib_dir "${rapidyaml_install_dir}/lib")

	durin_third_party_find_single_file(rapidyaml_library "${rapidyaml_lib_dir}"
		"ryml.lib"
		"rapidyaml.lib"
		"libryml.a"
		"librapidyaml.a"
	)

	if(NOT EXISTS "${rapidyaml_include_dir}/ryml.hpp" OR NOT rapidyaml_library)
		durin_third_party_get_arch(durin_arch)
		message(FATAL_ERROR
			"Installed rapidyaml package was not found for ${durin_arch} ${CMAKE_BUILD_TYPE}.\n"
			"Expected install root: ${rapidyaml_install_dir}\n"
			"Prepare rapidyaml first, for example:\n"
			"  Engine/Scripts/Bootstrap/Setup_rapidyaml.bat ${CMAKE_BUILD_TYPE}\n"
			"or use an equivalent cmake -S/-B/-DCMAKE_INSTALL_PREFIX flow."
		)
	endif()

	add_library(ryml STATIC IMPORTED GLOBAL)
	set_target_properties(ryml PROPERTIES
		IMPORTED_LOCATION "${rapidyaml_library}"
		INTERFACE_INCLUDE_DIRECTORIES "${rapidyaml_include_dir}"
	)

	add_library(ryml::ryml ALIAS ryml)
endfunction()

function(durin_rapidyaml_configure_source_build)
	project(DurinThirdPartyRapidyaml LANGUAGES C CXX)

	get_filename_component(durin_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
	set(rapidyaml_source_dir "${durin_root}/Engine/External/Source/rapidyaml")

	if(NOT EXISTS "${rapidyaml_source_dir}/CMakeLists.txt")
		message(FATAL_ERROR
			"rapidyaml source was not found at ${rapidyaml_source_dir}.\n"
			"Run Engine/Scripts/Bootstrap/Setup_rapidyaml.bat first to clone the source."
		)
	endif()

	if(MSVC)
		add_compile_options(/utf-8)
	endif()

	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(RYML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(RYML_STANDALONE ON CACHE BOOL "" FORCE)
	set(RYML_INSTALL ON CACHE BOOL "" FORCE)
	set(RYML_DEV OFF CACHE BOOL "" FORCE)
	set(RAPIDYAML_BUILD_TESTS OFF CACHE BOOL "" FORCE)

	add_subdirectory("${rapidyaml_source_dir}" "${CMAKE_BINARY_DIR}/rapidyaml")
endfunction()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	durin_rapidyaml_configure_source_build()
	return()
endif()

durin_rapidyaml_configure_imported_target()
