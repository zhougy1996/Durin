cmake_minimum_required(VERSION 3.24)

function(durin_glfw_configure_imported_target)
	durin_third_party_get_install_dir(glfw_install_dir "glfw")
	set(glfw_include_dir "${glfw_install_dir}/include")
	set(glfw_lib_dir "${glfw_install_dir}/lib")

	durin_third_party_find_single_file(glfw_library "${glfw_lib_dir}"
		"glfw3.lib"
		"glfw.lib"
		"libglfw3.a"
		"libglfw.a"
	)

	if(NOT EXISTS "${glfw_include_dir}/GLFW/glfw3.h" OR NOT glfw_library)
		durin_third_party_get_arch(durin_arch)
		message(FATAL_ERROR
			"Installed GLFW package was not found for ${durin_arch} ${CMAKE_BUILD_TYPE}.\n"
			"Expected install root: ${glfw_install_dir}\n"
			"Prepare GLFW first, for example:\n"
			"  Engine/Scripts/Bootstrap/Setup_glfw.bat ${CMAKE_BUILD_TYPE}\n"
			"or use an equivalent cmake -S/-B/-DCMAKE_INSTALL_PREFIX flow."
		)
	endif()

	add_library(glfw STATIC IMPORTED GLOBAL)
	set_target_properties(glfw PROPERTIES
		IMPORTED_LOCATION "${glfw_library}"
		INTERFACE_INCLUDE_DIRECTORIES "${glfw_include_dir}"
	)
endfunction()

function(durin_glfw_configure_source_build)
	project(DurinThirdPartyGlfw LANGUAGES C)

	get_filename_component(durin_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
	set(glfw_source_dir "${durin_root}/Engine/External/Source/glfw")

	if(NOT EXISTS "${glfw_source_dir}/CMakeLists.txt")
		message(FATAL_ERROR
			"GLFW source was not found at ${glfw_source_dir}.\n"
			"Run Engine/Scripts/Bootstrap/Setup_glfw.bat first to clone the source."
		)
	endif()

	set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
	set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(GLFW_INSTALL ON CACHE BOOL "" FORCE)
	set(GLFW_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

	add_subdirectory("${glfw_source_dir}" "${CMAKE_BINARY_DIR}/glfw")
endfunction()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	durin_glfw_configure_source_build()
	return()
endif()

durin_glfw_configure_imported_target()
