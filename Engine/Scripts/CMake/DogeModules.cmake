# DHT (Doge Header Tool) Integration

set(PYTHON_EXE python)

set(DHT_Dir ${DOGE_ENGINE_SOURCE_DIR}/Programs/DogeHeaderTool)
# set(DHT_EXE "${DHT_Dir}/dht.py")
set(DOGE_BUILD_TOOL ${PYTHON_EXE} "${DHT_Dir}/build_tool.py")

# Collect module information for the project (Engine, User custom Game projects, etc.)
function(doge_add_project project_name)
	message("- Project: ${project_name}")
	set(dproject_file ${CMAKE_CURRENT_SOURCE_DIR}/${project_name}.dproject)

	# set global project variables
	set(DOGE_PROJECT_NAME ${project_name})
	set(DOGE_PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
	set(DOGE_PROJECT_CONFIG_DIR "${DOGE_PROJECT_DIR}/Configs")
	set(DOGE_PROJECT_SCRIPT_DIR "${DOGE_PROJECT_DIR}/Scripts")
	set(DOGE_PROJECT_SOURCE_DIR "${DOGE_PROJECT_DIR}/Source")
	set(DOGE_PROJECT_BINARY_DIR "${DOGE_PROJECT_DIR}/Binaries")
	set(DOGE_PROJECT_INTERMEDIATE_DIR "${DOGE_PROJECT_DIR}/Intermediate")

	# Make CMake re-configure if the project definition file changes
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		${dproject_file}
	)

	execute_process(
		COMMAND ${DOGE_BUILD_TOOL} get_module_dirs -p ${dproject_file}
		OUTPUT_VARIABLE MODULE_DIRS
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)

	foreach(dir IN LISTS MODULE_DIRS)
		add_subdirectory(${dir})
	endforeach()
endfunction()

# Module Setup Functions
function(doge_set_module_output target)
	set_target_properties(${target} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${DOGE_PROJECT_BINARY_DIR}/Doge/${DOGE_ARCH}/$<CONFIG>"
		LIBRARY_OUTPUT_DIRECTORY "${DOGE_PROJECT_BINARY_DIR}/${target}/${DOGE_ARCH}/$<CONFIG>"
		ARCHIVE_OUTPUT_DIRECTORY "${DOGE_PROJECT_BINARY_DIR}/${target}/${DOGE_ARCH}/$<CONFIG>"
		PDB_OUTPUT_DIRECTORY "${DOGE_PROJECT_BINARY_DIR}/${target}/${DOGE_ARCH}/$<CONFIG>"
	)
endfunction()

function(doge_organize_source_files)
	foreach(source ${ARGV})
		file(RELATIVE_PATH relative_path ${CMAKE_CURRENT_SOURCE_DIR} ${source})
		get_filename_component(group_dir ${relative_path} DIRECTORY)

		if(NOT group_dir)
			set(group_name "Source Files")
		else()
			string(REPLACE "/" "\\" group_name ${group_dir})
		endif()

		source_group("${group_name}" FILES ${source})
	endforeach()
endfunction()

function(doge_collect_and_organize_source_files OUT_SRCS)
	set(patterns ${ARGN})
		if(NOT patterns)
			set(patterns 
				"*.h" "*.hpp" "*.hxx"
				"*.c" "*.cpp" "*.cxx" "*.cc"
			)
	endif()
	file(GLOB_RECURSE all_sources CONFIGURE_DEPENDS ${patterns})
	doge_organize_source_files(${all_sources})
	set(${OUT_SRCS} ${all_sources} PARENT_SCOPE)
endfunction()

function(doge_setup_shared_library module_name)
	doge_set_module_output(${module_name})
	set(module_dht_dir "${DOGE_PROJECT_INTERMEDIATE_DIR}/${module_name}/${DOGE_ARCH}/DHT")
	set_target_properties(${module_name} PROPERTIES OUTPUT_NAME "DogeEditor-${module_name}")
	string(TOUPPER "${module_name}" uppercase_module_name)

	target_compile_definitions(${module_name} PRIVATE ${uppercase_module_name}_EXPORTS)
	target_compile_definitions(${module_name} PRIVATE MODULE_NAME="${module_name}")

	target_include_directories(${module_name} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/Private
	)
	target_include_directories(${module_name} PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/Public
		${module_dht_dir}
	)
	target_precompile_headers(${module_name} PRIVATE
		"$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Private/PCH.${module_name}.h>"
	)
	install(FILES $<TARGET_FILE:${module_name}> DESTINATION bin)
endfunction()

function(json_get_string_list out_var json_string member)
	string(JSON _json_array_string ERROR_VARIABLE _error GET "${json_string}" ${member})

	if(_error)
		set(_json_array_string "[]")
	endif()

	string(JSON _len LENGTH "${_json_array_string}")
	set(_result "")
	if(_len GREATER 0)
		math(EXPR _last_index "${_len} - 1")
		foreach(i RANGE ${_last_index})
			string(JSON _item GET "${_json_array_string}" ${i})
			list(APPEND _result "${_item}")
		endforeach()
	endif()

	set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

function(doge_add_module module_name)
	message("-- Module: ${module_name}")

	set(module_file ${CMAKE_CURRENT_SOURCE_DIR}/${module_name}.dmodule)
	set(module_dht_dir "${DOGE_PROJECT_INTERMEDIATE_DIR}/${module_name}/${DOGE_ARCH}/DHT")

	# Make CMake re-configure if the module definition file changes
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${module_file})

	file(READ ${module_file} module_json)

	# Parse the module file (JSON format)
	json_get_string_list(private_dependencies "${module_json}" PrivateDependencies) # List of private dependencies
	json_get_string_list(dht_headers "${module_json}" DHTHeaders) # List of DHT headers

	# Collect DHT generated files
	set(dht_generated_files "")
	foreach(header ${dht_headers})
		get_filename_component(header_name ${header} NAME_WE)
		list(APPEND dht_generated_files ${module_dht_dir}/${header_name}.gen.h)
		list(APPEND dht_generated_files ${module_dht_dir}/${header_name}.gen.cpp)
	endforeach()

	doge_collect_and_organize_source_files(module_srcs)
	add_library(${module_name} SHARED
		${module_srcs}
		${dht_generated_files}
	)
	doge_setup_shared_library(${module_name})

	target_link_libraries(${module_name} PRIVATE
		${private_dependencies}
	)

	get_filename_component(module_folder "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
	set_target_properties(${module_name} PROPERTIES FOLDER "${DOGE_PROJECT_NAME}/${module_folder}")
endfunction()