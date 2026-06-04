# Project-level build entry helpers that load generated metadata and register modules.

include_guard(GLOBAL)

set(DHT_DIR ${DURIN_WORKSPACE_DIR}/Engine/Source/Programs/DurinHeaderTool)
set(DHT_MAIN ${Python_EXECUTABLE} "${DHT_DIR}/main.py")

function(durin_project_log project_name)
	message(STATUS "[Durin] Project: ${project_name}")
endfunction()

function(add_durin_project project_name)
	durin_project_log(${project_name})
	durin_start("Project_${project_name}")
	project(${project_name})

	set(DURIN_PROJECT_NAME "${project_name}" PARENT_SCOPE)
	set(_durin_project_intermediate_build_dir "${CMAKE_CURRENT_SOURCE_DIR}/Intermediate/Build/${DURIN_TARGET_PLATFORM}/${DURIN_PROFILE_NAME}")
	set(_durin_project_cmake_file "${_durin_project_intermediate_build_dir}/${project_name}.project.cmake")
	execute_process(
		COMMAND ${DHT_MAIN} prepare_project_build -p ${project_name} -a ${DURIN_TARGET_PLATFORM} --profile ${DURIN_PROFILE_NAME}
		RESULT_VARIABLE _durin_prepare_project_build_result
	)
	if(NOT _durin_prepare_project_build_result EQUAL 0)
		message(FATAL_ERROR "Failed to prepare build metadata for project ${project_name} and profile ${DURIN_PROFILE_NAME}.")
	endif()

	include("${_durin_project_cmake_file}")

	set(DURIN_PROJECT_NAME "${project_name}")
	if(NOT DEFINED DURIN_PROJECT_ROOT_DIR)
		set(DURIN_PROJECT_ROOT_DIR "${DURIN_PROJECT_DIR}")
	endif()
	if(NOT DEFINED DURIN_PROJECT_SOURCE_DIR)
		set(DURIN_PROJECT_SOURCE_DIR "${DURIN_PROJECT_ROOT_DIR}/Source")
	endif()
	if(NOT DEFINED DURIN_PROJECT_CMAKE_DIR)
		set(DURIN_PROJECT_CMAKE_DIR "${DURIN_PROJECT_ROOT_DIR}/CMake")
	endif()

	set(DURIN_PROJECT_DIR "${DURIN_PROJECT_ROOT_DIR}")

	set(_durin_project_shared_pch_file "${DURIN_PROJECT_CMAKE_DIR}/SharedPCH/SharedPCH.cmake")
	if(EXISTS "${_durin_project_shared_pch_file}")
		include("${_durin_project_shared_pch_file}")
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_durin_project_shared_pch_file}")
	endif()

	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DURIN_PROJECT_CONFIG_FILE}")

	foreach(_durin_module_dir IN LISTS DURIN_PROJECT_MODULE_DIRS)
		add_subdirectory(${_durin_module_dir})
	endforeach()

	if(BUILD_TESTING)
		set(_durin_project_tests_dir "${DURIN_PROJECT_SOURCE_DIR}/Programs/Tests")
		if(EXISTS "${_durin_project_tests_dir}/CMakeLists.txt")
			add_subdirectory("${_durin_project_tests_dir}")
		endif()
	endif()
	durin_end()
endfunction()
