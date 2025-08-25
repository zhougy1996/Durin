# DHT (Doge Header Tool) Integration
set(DHT_EXE "${DOGE_SCRIPT_DIR}/TestParse.py")

function (doge_module_get_dht_output_directory module_name out_directory)
	set(_dht_output_directory "${DOGE_INTERMEDIATE_DIR}/${module_name}/${DOGE_ARCH}/DHT")
	set(${out_directory} ${_dht_output_directory} PARENT_SCOPE)
endfunction()

function (doge_set_dht_input_headers input_headers output_directory out_generated_files)
	message("input_headers: " ${input_headers})
	set(_all_generated_files "")
	foreach(header ${input_headers})
		message("DHT Input Header: ${header}")
		get_filename_component(header_name ${header} NAME_WE)
		set(_generated_files
			${output_directory}/${header_name}.gen.h
			${output_directory}/${header_name}.gen.cpp
		)

		add_custom_command(
			OUTPUT ${_generated_files}
			COMMAND python "${DHT_EXE}" "${header}" "${output_directory}"
			DEPENDS ${header} ${DHT_EXE}
			COMMENT "DHT Parsing ${header_name}.h"
			VERBATIM
		)
		list(APPEND _all_generated_files ${_generated_files})
	endforeach()
	set(${out_generated_files} ${_all_generated_files} PARENT_SCOPE)
endfunction()

function (doge_module_add_dht_input_headers module_name input_headers out_generated_files)

	doge_module_get_dht_output_directory(${module_name} _dht_output_directory)
	doge_set_dht_input_headers("${input_headers}" ${_dht_output_directory} _generated_files)

	set(${out_generated_files} ${_generated_files} PARENT_SCOPE)	
endfunction()

# Module Setup Functions
function(doge_set_module_output target)
	set_target_properties(${target} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${DOGE_BINARY_DIR}/Doge/${DOGE_ARCH}/$<CONFIG>"
		LIBRARY_OUTPUT_DIRECTORY "${DOGE_BINARY_DIR}/${target}/${DOGE_ARCH}/$<CONFIG>"
		ARCHIVE_OUTPUT_DIRECTORY "${DOGE_BINARY_DIR}/${target}/${DOGE_ARCH}/$<CONFIG>"
		PDB_OUTPUT_DIRECTORY "${DOGE_BINARY_DIR}/${target}/${DOGE_ARCH}/$<CONFIG>"
	)
endfunction()

function(doge_print_project_build_info)
	message("-- Build ${PROJECT_NAME}")
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
