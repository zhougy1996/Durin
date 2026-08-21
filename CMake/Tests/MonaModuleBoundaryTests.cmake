cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED DURIN_WORKSPACE_DIR)
	message(FATAL_ERROR "DURIN_WORKSPACE_DIR is required")
endif()

function(durin_read_module_descriptor module_name out_json)
	file(GLOB_RECURSE descriptor_candidates
		"${DURIN_WORKSPACE_DIR}/Engine/Source/*/${module_name}/${module_name}.dmodule")
	list(LENGTH descriptor_candidates descriptor_count)
	if(NOT descriptor_count EQUAL 1)
		message(FATAL_ERROR
			"Expected one descriptor for ${module_name}, found ${descriptor_count}")
	endif()
	list(GET descriptor_candidates 0 descriptor_path)
	file(READ "${descriptor_path}" descriptor_json)
	set(${out_json} "${descriptor_json}" PARENT_SCOPE)
endfunction()

function(durin_assert_dependency descriptor_json dependency_kind dependency expected)
	string(JSON dependency_count ERROR_VARIABLE dependency_error
		LENGTH "${descriptor_json}" "${dependency_kind}")
	if(dependency_error)
		set(dependency_count 0)
	endif()
	set(found FALSE)
	if(dependency_count GREATER 0)
		math(EXPR last_index "${dependency_count} - 1")
		foreach(index RANGE 0 ${last_index})
			string(JSON candidate GET "${descriptor_json}" "${dependency_kind}" ${index})
			if(candidate STREQUAL dependency)
				set(found TRUE)
			endif()
		endforeach()
	endif()
	if(expected AND NOT found)
		message(FATAL_ERROR "Expected ${dependency_kind} to contain ${dependency}")
	endif()
	if(NOT expected AND found)
		message(FATAL_ERROR "Expected ${dependency_kind} not to contain ${dependency}")
	endif()
endfunction()

durin_read_module_descriptor(Engine engine_descriptor)
durin_assert_dependency("${engine_descriptor}" PublicDependencies MonaCore TRUE)
durin_assert_dependency("${engine_descriptor}" PrivateDependencies Mona TRUE)

durin_read_module_descriptor(Mona mona_descriptor)
durin_assert_dependency("${mona_descriptor}" PublicDependencies Engine FALSE)
durin_assert_dependency("${mona_descriptor}" PrivateDependencies Engine FALSE)
durin_assert_dependency("${mona_descriptor}" PublicDependencies MonaImGui FALSE)
durin_assert_dependency("${mona_descriptor}" PrivateDependencies MonaImGui FALSE)

durin_read_module_descriptor(MonaCore mona_core_descriptor)
durin_assert_dependency("${mona_core_descriptor}" PublicDependencies Engine FALSE)
durin_assert_dependency("${mona_core_descriptor}" PrivateDependencies Engine FALSE)

file(GLOB_RECURSE mona_sources
	"${DURIN_WORKSPACE_DIR}/Engine/Source/Runtime/Mona/*.h"
	"${DURIN_WORKSPACE_DIR}/Engine/Source/Runtime/Mona/*.cpp")
foreach(source_path IN LISTS mona_sources)
	file(READ "${source_path}" source_text)
	string(FIND "${source_text}" "MonaImGui" backend_name_offset)
	if(NOT backend_name_offset EQUAL -1)
		message(FATAL_ERROR
			"Generic Mona source names the concrete MonaImGui backend: ${source_path}")
	endif()
endforeach()
