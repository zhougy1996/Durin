# DHT (Doge Header Tool) Integration

set(DHT_DIR ${DOGE_ENGINE_SOURCE_DIR}/Programs/DogeHeaderTool)
set(DHT_MAIN ${Python_EXECUTABLE} "${DHT_DIR}/main.py")

# Collect module information for the project (Engine, User custom Game projects, etc.)
function(doge_add_project project_name)
	message("-- Project: ${project_name}")
	set(DOGE_PROJECT_INTERMEDIATE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Intermediate/Build/${DOGE_ARCH}/Editor")

	set(project_cmake_file "${DOGE_PROJECT_INTERMEDIATE_BUILD_DIR}/${project_name}.project.cmake")
	execute_process(COMMAND ${DHT_MAIN} generate_project_cmake_file -p ${project_name} -a ${DOGE_ARCH})
	include(${project_cmake_file})

	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${project_config_file}) # Make CMake re-configure if the project definition file changes

	# Add subdirectories for each module
	foreach(_dir IN LISTS project_module_dirs)
		add_subdirectory(${_dir})
	endforeach()
endfunction()

# Module Setup Functions
function(doge_set_module_output target)
	set_target_properties(${target} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${DOGE_PROJECT_BINARY_DIR}/Doge/${DOGE_ARCH}/$<CONFIG>"
		LIBRARY_OUTPUT_DIRECTORY "${DOGE_PROJECT_BINARY_DIR}/Doge/${DOGE_ARCH}/$<CONFIG>"
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

function(doge_add_module module_name)
	# Generate module CMake file using DHT and include it to get module-specific variables
	set(module_cmake_file "${DOGE_PROJECT_INTERMEDIATE_BUILD_DIR}/${module_name}/${module_name}.module.cmake")
	execute_process(COMMAND ${DHT_MAIN} generate_module_cmake_file -m ${module_name} -a ${DOGE_ARCH})
	include(${module_cmake_file})

	message("--- Module: ${module_name}")

	# Make CMake re-configure if the module definition file changes
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${module_config_file})

	doge_collect_and_organize_source_files(module_srcs)

	set(generated_reflection_files)
	if (module_reflect_headers)
		add_custom_command(
			OUTPUT ${module_export_file}
			COMMAND ${DHT_MAIN} generate_module_export_file -m ${module_name} -a ${DOGE_ARCH}
			DEPENDS ${module_reflect_headers} ${module_cmake_file}
		)
		# Collect generated reflection files for the module
		foreach(header ${module_reflect_headers})
			get_filename_component(header_name ${header} NAME_WE)
			list(APPEND generated_reflection_files ${module_dht_output_dir}/${header_name}.gen.h)
			list(APPEND generated_reflection_files ${module_dht_output_dir}/${header_name}.gen.cpp)
		endforeach()
		list(APPEND generated_reflection_files ${module_dht_output_dir}/${module_name}.module.gen.cpp)

		add_custom_command(
			OUTPUT ${generated_reflection_files}
			COMMAND ${DHT_MAIN} generate_reflection_files -m ${module_name} -a ${DOGE_ARCH}
			DEPENDS ${module_cmake_file} ${module_manifest_dependencies} ${module_export_file}
		)
	endif()

	if("${module_link_type}" STREQUAL "STATIC")
		set(module_link_type_final STATIC)
	else()
		set(module_link_type_final SHARED)
	endif()
	add_library(${module_name} ${module_link_type_final}
		${module_srcs}
		${generated_reflection_files}
	)

	set_target_properties(${module_name} PROPERTIES OUTPUT_NAME "DogeEditor-${module_name}")

	string(TOUPPER "${module_name}" uppercase_module_name)
	target_compile_definitions(${module_name} PRIVATE ${uppercase_module_name}_EXPORTS)

	target_include_directories(${module_name} PRIVATE
		${project_intermediate_build_dir}
		${CMAKE_CURRENT_SOURCE_DIR}/Private
	)

	target_include_directories(${module_name} PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/Public
		${module_dht_output_dir}
	)

	target_link_libraries(${module_name} PRIVATE
		${module_private_dependencies}
	)

	# Set up precompiled headers for the module
	target_precompile_headers(${module_name} PRIVATE
		"$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Private/PCH.${module_name}.h>"
	)

	doge_set_module_output(${module_name})
	# Organize the module in the IDE's folder structure
	set_target_properties(${module_name} PROPERTIES FOLDER "${project_name}/${module_dir}")
endfunction()