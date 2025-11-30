# DHT (Doge Header Tool) Integration

set(PYTHON_EXECUTABLE python)

set(DHT_Dir ${DOGE_ENGINE_SOURCE_DIR}/Programs/DogeHeaderTool)
set(DHT_EXE "${DHT_Dir}/dht.py")
set(DHT_MODULE_TOOLS_EXE "${DHT_Dir}/module_tools.py")
set(DHT_PREPARE_MODULE_INTO_EXE "${DHT_Dir}/dbt.py")

set(DOGE_BUILD_TOOL_EXE "${DOGE_ENGINE_SCRIPT_DIR}/DogeBuildTool/dbt.py")

# Collect module information for the project (Engine, User custom Game projects, etc.)
function(doge_add_project project_name)
	message("- Project: ${project_name}")

	set(dproject_file ${CMAKE_CURRENT_SOURCE_DIR}/${project_name}.dproject)
	# Make CMake re-configure if the project definition file changes
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		${dproject_file}
	)
	file(READ ${dproject_file} project_json)

	# Parse the project file (JSON format)
	string(JSON module_count LENGTH "${project_json}" Modules)
	message(STATUS "Module count of project \"${project_name}\": ${module_count}")

	math(EXPR last_index "${module_count} - 1")

	foreach(index RANGE 0 ${last_index})
		# Get single module object
		string(JSON module_json GET "${project_json}" Modules ${index})

		string(JSON module_name GET "${module_json}" Name)
		string(JSON module_path GET "${module_json}" Path)

		if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${module_path}")
			message(FATAL_ERROR "Module path not found: ${module_path}")
		endif()

		add_subdirectory(${module_path})

		# Organize modules into folders in IDEs
		get_filename_component(module_folder "${module_path}" DIRECTORY)
		set_target_properties(${module_name} PROPERTIES FOLDER "${project_name}/${module_folder}")
	endforeach()

	# set global variables
	set(DOGE_PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
	set(DOGE_PROJECT_CONFIG_DIR "${DOGE_PROJECT_DIR}/Configs" PARENT_SCOPE)
	set(DOGE_PROJECT_SCRIPT_DIR "${DOGE_PROJECT_DIR}/Scripts" PARENT_SCOPE)
	set(DOGE_PROJECT_SOURCE_DIR "${DOGE_PROJECT_DIR}/Source" PARENT_SCOPE)
	set(DOGE_PROJECT_BINARY_DIR "${DOGE_PROJECT_DIR}/Binaries" PARENT_SCOPE)
	set(DOGE_PROJECT_INTERMEDIATE_DIR "${DOGE_PROJECT_DIR}/Intermediate" PARENT_SCOPE)
endfunction()

function (doge_module_get_dht_output_directory module_name out_directory)
	set(_dht_output_directory "${DOGE_PROJECT_INTERMEDIATE_DIR}/${module_name}/${DOGE_ARCH}/DHT")
	set(${out_directory} ${_dht_output_directory} PARENT_SCOPE)
endfunction()

function (doge_target_set_dht_headers module_name out_generated_files)
	doge_module_get_dht_output_directory(${module_name} _dht_output_directory)

	execute_process(
		COMMAND ${PYTHON_EXECUTABLE} ${DHT_MODULE_TOOLS_EXE} --module_dir ${CMAKE_CURRENT_SOURCE_DIR} --function get_dht_headers
		OUTPUT_VARIABLE dht_input_headers
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)

	set(_all_generated_files "")

	foreach(header ${dht_input_headers})
		get_filename_component(header_name ${header} NAME_WE)
		set(_generated_files
			${_dht_output_directory}/${header_name}.gen.h
			${_dht_output_directory}/${header_name}.gen.cpp
		)
		list(APPEND _all_generated_files ${_generated_files})
	endforeach()

	set(${out_generated_files} ${_all_generated_files} PARENT_SCOPE)
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

function(doge_print_project_build_info)
	message("-- Build a module //  will be replaced by add module")
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
	doge_module_get_dht_output_directory(${module_name} _dht_output_directory)
	set_target_properties(${module_name} PROPERTIES OUTPUT_NAME "DogeEditor-${module_name}")
	string(TOUPPER "${module_name}" uppercase_module_name)

	target_compile_definitions(${module_name} PRIVATE ${uppercase_module_name}_EXPORTS)
	target_compile_definitions(${module_name} PRIVATE MODULE_NAME="${module_name}")

	target_include_directories(${module_name} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/Private
	)
	target_include_directories(${module_name} PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/Public
		${_dht_output_directory}
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

	# Make CMake re-configure if the module definition file changes
	set(_module_file_path ${CMAKE_CURRENT_SOURCE_DIR}/${module_name}.dmodule)

	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		${_module_file_path}
	)

	file(READ ${_module_file_path} _module_file_content)

	# Parse the module file (JSON format)
	json_get_string_list(_private_dependencies "${_module_file_content}" PrivateDependencies) # List of private dependencies

	doge_collect_and_organize_source_files(_srcs)
	doge_target_set_dht_headers(${module_name} _dht_generated_files)

	add_library(${module_name} SHARED
			${_srcs}
			${_dht_generated_files}
	)
	doge_setup_shared_library(${module_name})

	target_link_libraries(${module_name} PRIVATE
		${_private_dependencies}
	)
endfunction()