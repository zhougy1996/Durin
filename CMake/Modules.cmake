include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/Timer.cmake)

set(DHT_DIR ${DOGE_DIR}/Engine/Source/Programs/DogeHeaderTool)
set(DHT_MAIN ${Python_EXECUTABLE} "${DHT_DIR}/main.py")

function(doge_log_project project_name)
	message(STATUS "[Doge] Project: ${project_name}")
endfunction()

function(doge_log_module project_name module_name)
	message(STATUS "[${project_name}] Module: ${module_name}")
endfunction()

# Collect module information for the project (Engine, User custom Game projects, etc.)
function(doge_add_project project_name)
	doge_log_project(${project_name})
	doge_start("Project_${project_name}")
	project(Engine)
	set(DOGE_PROJECT_INTERMEDIATE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Intermediate/Build/${DOGE_ARCH}/Editor")

	set(project_cmake_file "${DOGE_PROJECT_INTERMEDIATE_BUILD_DIR}/${project_name}.project.cmake")
	execute_process(COMMAND ${DHT_MAIN} prepare_project_build -p ${project_name} -a ${DOGE_ARCH})
	include(${project_cmake_file})

	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${project_config_file}) # Make CMake re-configure if the project definition file changes

	# Add subdirectories for each module
	foreach(_dir IN LISTS project_module_dirs)
		add_subdirectory(${_dir})
	endforeach()
	doge_end()
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
	doge_log_module(${project_name} ${module_name})
	doge_start("Module_${module_name}")

	set(module_cmake_file "${DOGE_PROJECT_INTERMEDIATE_BUILD_DIR}/${module_name}/${module_name}.module.cmake")
	include(${module_cmake_file})

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

	target_compile_definitions(${module_name} PRIVATE MODULE_NAME="${module_name}")
	if("${module_link_type_final}" STREQUAL "SHARED")
		string(TOUPPER "${module_name}" uppercase_module_name)
	endif()

	target_compile_definitions(${module_name} PRIVATE
		$<$<CONFIG:Debug>:DOGE_BUILD_DEBUG=1>
		$<$<CONFIG:Release>:DOGE_BUILD_RELEASE=1>
	)

	target_include_directories(${module_name} PRIVATE
		${project_intermediate_build_dir}
		${CMAKE_CURRENT_SOURCE_DIR}/Private
	)

	target_include_directories(${module_name} PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/Public
		${module_dht_output_dir}
	)

	target_link_libraries(${module_name} PRIVATE ${module_private_dependencies})
	target_link_libraries(${module_name} PUBLIC ${module_public_dependencies})

	# Set up precompiled headers for the module
	if(module_pch_target)
		target_precompile_headers(${module_name} REUSE_FROM ${module_pch_target})
		target_link_libraries(${module_name} PRIVATE ${module_pch_target})
	else()
		target_precompile_headers(${module_name} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Private/PCH.${module_name}.h>")
	endif()

	doge_set_module_output(${module_name})
	# Organize the module in the IDE's folder structure
	set_target_properties(${module_name} PROPERTIES FOLDER "${project_name}/${module_dir}")

	doge_end() # ${module_name}
endfunction()

function(doge_copy_external_target_binary target dependent_target)
	get_filename_component(file_name "$<TARGET_FILE_NAME:${dependent_target}>" NAME)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:${dependent_target}>"
		"${DOGE_PROJECT_EXTERNAL_RUNTIME_DIR}/${file_name}"
		COMMENT "Deploying target binary: ${file_name}"
		VERBATIM
	)
endfunction()

function(doge_copy_external_binaries target file_list)
	foreach(file_path ${file_list})
		get_filename_component(file_name "${file_path}" NAME)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${file_path}"
			"${DOGE_PROJECT_EXTERNAL_RUNTIME_DIR}/${file_name}"
			COMMENT "Deploying external file: ${file_name}"
			VERBATIM
		)
	endforeach()
endfunction()
