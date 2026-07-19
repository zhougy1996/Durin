# Project-level build entry helpers that load generated metadata and register modules.

include_guard(GLOBAL)

set(DHT_DIR ${DURIN_WORKSPACE_DIR}/Engine/Source/Programs/DurinHeaderTool)
set(DHT_MAIN ${Python_EXECUTABLE} "${DHT_DIR}/durin_header_tool/__main__.py")

# DHT runs both while configuring projects and from build-time custom commands.
# Track the complete implementation contract here so either path is invalidated
# when the tool changes, without relying on a manually maintained version file.
file(GLOB_RECURSE DURIN_DHT_TOOL_INPUTS CONFIGURE_DEPENDS
	LIST_DIRECTORIES FALSE
	"${DHT_DIR}/durin_header_tool/*.py"
)
list(APPEND DURIN_DHT_TOOL_INPUTS "${DURIN_WORKSPACE_DIR}/requirements.txt")
list(SORT DURIN_DHT_TOOL_INPUTS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DURIN_DHT_TOOL_INPUTS})

set(_durin_dht_fingerprint_content "")
foreach(_durin_dht_input IN LISTS DURIN_DHT_TOOL_INPUTS)
	file(RELATIVE_PATH _durin_dht_input_relative "${DURIN_WORKSPACE_DIR}" "${_durin_dht_input}")
	file(SHA256 "${_durin_dht_input}" _durin_dht_input_hash)
	string(APPEND _durin_dht_fingerprint_content "${_durin_dht_input_relative}:${_durin_dht_input_hash}\n")
endforeach()
string(SHA256 DURIN_DHT_TOOL_FINGERPRINT "${_durin_dht_fingerprint_content}")
set(DURIN_DHT_TOOL_FINGERPRINT_FILE "${CMAKE_BINARY_DIR}/DHT.fingerprint")
file(CONFIGURE
	OUTPUT "${DURIN_DHT_TOOL_FINGERPRINT_FILE}"
	CONTENT "${DURIN_DHT_TOOL_FINGERPRINT}\n"
	@ONLY
)

function(durin_project_log project_name)
	message(STATUS "[${project_name}] Project: ${project_name}")
endfunction()

function(add_durin_project project_name)
	durin_project_log(${project_name})
	durin_start("Project_${project_name}")
	project(${project_name})

	set(DURIN_PROJECT_NAME "${project_name}" PARENT_SCOPE)
	# DHT metadata is configuration-independent; the identifier isolates workflow ownership.
	set(_durin_intermediate_build_root "Build")
	if(DURIN_BUILD_IDENTIFIER)
		string(APPEND _durin_intermediate_build_root "-${DURIN_BUILD_IDENTIFIER}")
	endif()
	set(_durin_project_intermediate_build_dir "${CMAKE_CURRENT_SOURCE_DIR}/Intermediate/${_durin_intermediate_build_root}/${DURIN_TARGET_PLATFORM}/${DURIN_PROFILE_NAME}")
	set(_durin_project_cmake_file "${_durin_project_intermediate_build_dir}/${project_name}.project.cmake")
	set(DURIN_DHT_CONTEXT_ARGS
		-a ${DURIN_TARGET_PLATFORM}
		--profile ${DURIN_PROFILE_NAME}
		--tool-fingerprint ${DURIN_DHT_TOOL_FINGERPRINT}
	)
	if(DURIN_BUILD_IDENTIFIER)
		list(APPEND DURIN_DHT_CONTEXT_ARGS --build-identifier ${DURIN_BUILD_IDENTIFIER})
	endif()
	execute_process(
		COMMAND ${DHT_MAIN} prepare_project_build -p "${CMAKE_CURRENT_SOURCE_DIR}/${project_name}.dproject" ${DURIN_DHT_CONTEXT_ARGS}
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
