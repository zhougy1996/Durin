# DHT (Doge Header Tool) Integration

set(PYTHON_EXE python)

set(DHT_Dir ${DOGE_ENGINE_SOURCE_DIR}/Programs/DogeHeaderTool)
# set(DHT_EXE "${DHT_Dir}/dht.py")
set(DBT_EXE ${PYTHON_EXE} "${DHT_Dir}/doge_build_tool.py")
set(DHT_MAIN ${PYTHON_EXE} "${DHT_Dir}/cli/main.py")

# Collect module information for the project (Engine, User custom Game projects, etc.)
function(doge_add_project project_name)
	message("-- Project: ${project_name}")
	set(dproject_file ${CMAKE_CURRENT_SOURCE_DIR}/${project_name}.dproject)

	# set global project variables
	set(DOGE_PROJECT_NAME ${project_name})
	set(DOGE_PROJECT_FILE ${dproject_file})
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
		COMMAND ${DBT_EXE} get_module_dirs -p ${dproject_file}
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

function(doge_setup_shared_library module_name module_dht_dir)
	doge_set_module_output(${module_name})
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

function(doge_add_module module_name)
	message("--- Module: ${module_name}")

	# Generate module CMake file using DHT and include it to get module-specific variables
	set(module_cmake_file "${DOGE_PROJECT_INTERMEDIATE_DIR}/Build/${DOGE_ARCH}/Editor/${module_name}/${module_name}.module.cmake")
	execute_process(
		COMMAND ${DHT_MAIN} generate_module_cmake_file -m ${module_name}
	)
	include(${module_cmake_file})

	# Make CMake re-configure if the module definition file changes
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${module_config_file})

	# Collect generated reflection files for the module
	set(module_generated_reflection_files)
	foreach(header ${module_reflect_headers})
		get_filename_component(header_name ${header} NAME_WE)
		list(APPEND generated_reflection_files ${module_dht_output_dir}/${header_name}.gen.h)
		list(APPEND generated_reflection_files ${module_dht_output_dir}/${header_name}.gen.cpp)
	endforeach()
	list(APPEND generated_reflection_files ${module_dht_output_dir}/${module_name}.module.gen.cpp)

	add_custom_command(
		OUTPUT ${module_manifest_file}
		COMMAND ${DBT_EXE} generate_module_manifest_file
		-p "${DOGE_PROJECT_FILE}"
		-m "${module_name}"
		DEPENDS ${module_reflect_headers} ${module_file} ${DOGE_PROJECT_FILE}
		WORKING_DIRECTORY ${DOGE_PROJECT_DIR}
	)

	add_custom_command(
		OUTPUT ${generated_reflection_files}
		COMMAND ${DBT_EXE} run_header_tool
		-p "${DOGE_PROJECT_FILE}"
		-m "${module_name}"
		DEPENDS ${module_manifest_file} ${module_dependency_manifests}
		WORKING_DIRECTORY ${DOGE_PROJECT_DIR}
		# COMMENT "Running DHT for module: ${module_name}"
	)

	doge_collect_and_organize_source_files(module_srcs)
	add_library(${module_name} SHARED
		${module_srcs}
		${generated_reflection_files}
	)
	doge_setup_shared_library(${module_name} ${module_dht_output_dir})

	target_link_libraries(${module_name} PRIVATE
		${module_private_dependencies}
	)

	get_filename_component(module_folder "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
	set_target_properties(${module_name} PROPERTIES FOLDER "${DOGE_PROJECT_NAME}/${module_folder}")
endfunction()