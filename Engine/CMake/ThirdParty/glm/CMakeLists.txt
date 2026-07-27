cmake_minimum_required(VERSION 3.24)

get_filename_component(durin_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(glm_source_dir "${durin_root}/Engine/External/Source/glm")

if(NOT EXISTS "${glm_source_dir}/CMakeLists.txt")
	message(FATAL_ERROR
		"GLM source was not found at ${glm_source_dir}.\n"
		"Run DevTool.bat dependency prepare --libs glm first."
	)
endif()

set(GLM_QUIET OFF CACHE BOOL "" FORCE)
set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)

add_subdirectory("${glm_source_dir}" "${CMAKE_BINARY_DIR}/glm")
