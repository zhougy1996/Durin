# Common workspace-wide build options and compiler defaults.

include_guard(GLOBAL)

set(FETCHCONTENT_QUIET OFF)

if(NOT DEFINED ENABLE_DURIN_TIMER)
	set(ENABLE_DURIN_TIMER OFF)
endif()

if(NOT DEFINED DURIN_ENABLE_PCH)
	set(DURIN_ENABLE_PCH ON CACHE BOOL "Enable precompiled headers for Durin targets.")
endif()

if(NOT DEFINED DURIN_FORCE_INCLUDE_PCH)
	set(DURIN_FORCE_INCLUDE_PCH ON CACHE BOOL "Force-include PCH headers even when PCH artifacts are disabled.")
endif()

option(DURIN_IDE_CODE_MODEL_ONLY "Generate IDE code-model metadata but reject every build target." OFF)

function(_durin_attach_code_model_build_guard directory guard_target)
	get_property(_durin_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
	foreach(_durin_target IN LISTS _durin_targets)
		if(_durin_target STREQUAL guard_target)
			continue()
		endif()

		get_target_property(_durin_target_imported "${_durin_target}" IMPORTED)
		if(NOT _durin_target_imported)
			add_dependencies("${_durin_target}" "${guard_target}")
		endif()
	endforeach()

	get_property(_durin_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
	foreach(_durin_subdirectory IN LISTS _durin_subdirectories)
		_durin_attach_code_model_build_guard("${_durin_subdirectory}" "${guard_target}")
	endforeach()
endfunction()

function(durin_enforce_code_model_only_build)
	if(NOT DURIN_IDE_CODE_MODEL_ONLY)
		return()
	endif()

	set(_durin_guard_target DurinCodeModelOnlyBuildGuard)
	add_custom_target(${_durin_guard_target} ALL
		COMMAND ${CMAKE_COMMAND} -E echo
			"ERROR: This IDE preset is code-model-only and cannot build. Use BuildTool.bat with a registered build preset."
		COMMAND ${CMAKE_COMMAND} -E false
		VERBATIM
	)
	_durin_attach_code_model_build_guard("${CMAKE_SOURCE_DIR}" "${_durin_guard_target}")
endfunction()

set(DURIN_BUILD_IDENTIFIER "" CACHE STRING "Optional identifier that isolates workflow-owned binary and intermediate outputs.")
if(DURIN_BUILD_IDENTIFIER AND NOT DURIN_BUILD_IDENTIFIER MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
	message(FATAL_ERROR
		"DURIN_BUILD_IDENTIFIER must start with an alphanumeric character and contain only alphanumeric characters, '.', '_' or '-'."
	)
endif()

set(DURIN_DHT_WORKERS 4 CACHE STRING "Maximum parser workers used inside one DHT command (1-8).")
if(NOT DURIN_DHT_WORKERS MATCHES "^[1-8]$")
	message(FATAL_ERROR "DURIN_DHT_WORKERS must be an integer from 1 to 8.")
endif()

set(DURIN_DHT_LOG_LEVEL INFO CACHE STRING "DHT log detail (DEBUG, INFO, WARNING, ERROR, or CRITICAL).")
set_property(CACHE DURIN_DHT_LOG_LEVEL PROPERTY STRINGS DEBUG INFO WARNING ERROR CRITICAL)
if(NOT DURIN_DHT_LOG_LEVEL MATCHES "^(DEBUG|INFO|WARNING|ERROR|CRITICAL)$")
	message(FATAL_ERROR "DURIN_DHT_LOG_LEVEL must be DEBUG, INFO, WARNING, ERROR, or CRITICAL.")
endif()

set(DURIN_DHT_JOB_POOL_SIZE 2 CACHE STRING "Maximum number of concurrent DHT commands scheduled by Ninja.")
if(NOT DURIN_DHT_JOB_POOL_SIZE MATCHES "^[1-9][0-9]*$")
	message(FATAL_ERROR "DURIN_DHT_JOB_POOL_SIZE must be a positive integer.")
endif()
# Ninja counts a Python parent as one job but cannot see its parser children.
# Keep only a small number of DHT commands active so their bounded worker pools
# coexist with compiler jobs without multiplying machine-wide parallelism.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS durin_dht=${DURIN_DHT_JOB_POOL_SIZE})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
add_definitions(-DUNICODE -D_UNICODE)

if(MSVC)
	add_compile_options(/MP)
	add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/utf-8>)
	add_compile_options(/Zc:preprocessor)
	add_compile_options(/FS)
endif()
