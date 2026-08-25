# Common workspace-wide build options and compiler defaults.

include_guard(GLOBAL)

set(FETCHCONTENT_QUIET OFF)

if(DEFINED DURIN_PROFILE_NAME)
	message(FATAL_ERROR
		"DURIN_PROFILE_NAME was replaced by DURIN_RUNTIME_VARIANT. "
		"Remove the legacy cache entry and configure with -DDURIN_RUNTIME_VARIANT=DurinEditor or DurinGame."
	)
endif()

set(DURIN_RUNTIME_VARIANT "DurinEditor" CACHE STRING "Workspace-wide runtime variant.")
set_property(CACHE DURIN_RUNTIME_VARIANT PROPERTY STRINGS DurinEditor DurinGame)
if(NOT DURIN_RUNTIME_VARIANT MATCHES "^(DurinEditor|DurinGame)$")
	message(FATAL_ERROR "DURIN_RUNTIME_VARIANT must be DurinEditor or DurinGame.")
endif()

option(DURIN_ENABLE_TRACY "Enable Tracy CPU profiling instrumentation." OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Shipping" AND DURIN_ENABLE_TRACY)
	message(FATAL_ERROR "DURIN_ENABLE_TRACY cannot be enabled for Shipping builds.")
endif()

option(DURIN_ENABLE_APPLICATION_TESTS
	"Enable macOS native tests that require LaunchServices application hosting."
	OFF)
if(DURIN_ENABLE_APPLICATION_TESTS AND NOT APPLE)
	message(FATAL_ERROR
		"DURIN_ENABLE_APPLICATION_TESTS is supported only on macOS.")
endif()
if(DURIN_ENABLE_TRACY)
	set(DURIN_WITH_TRACY 1)
else()
	set(DURIN_WITH_TRACY 0)
endif()

set(DURIN_PRESET_ROLE "Standard" CACHE STRING "Operational role of the active preset.")
set_property(CACHE DURIN_PRESET_ROLE PROPERTY STRINGS Standard Profiling)
if(NOT DURIN_PRESET_ROLE MATCHES "^(Standard|Profiling)$")
	message(FATAL_ERROR "DURIN_PRESET_ROLE must be Standard or Profiling.")
endif()
set(DURIN_OUTPUT_CONFIG "$<CONFIG>")
if(DURIN_PRESET_ROLE STREQUAL "Profiling")
	string(APPEND DURIN_OUTPUT_CONFIG "-Profiling")
endif()
set(DURIN_THIRDPARTY_OUTPUT_CONFIG "$<CONFIG>")
message(STATUS
	"Durin build: runtime variant=${DURIN_RUNTIME_VARIANT}, "
	"configuration=${CMAKE_BUILD_TYPE}, preset role=${DURIN_PRESET_ROLE}, "
	"Tracy=${DURIN_ENABLE_TRACY}, application tests=${DURIN_ENABLE_APPLICATION_TESTS}"
)

if(NOT DEFINED ENABLE_DURIN_TIMER)
	set(ENABLE_DURIN_TIMER OFF)
endif()

if(NOT DEFINED DURIN_ENABLE_PCH)
	set(DURIN_ENABLE_PCH ON CACHE BOOL "Enable precompiled headers for Durin targets.")
endif()

if(NOT DEFINED DURIN_FORCE_INCLUDE_PCH)
	set(DURIN_FORCE_INCLUDE_PCH ON CACHE BOOL "Force-include PCH headers even when PCH artifacts are disabled.")
endif()

option(DURIN_ENABLE_UNITY_BUILD "Combine Durin module sources into bounded unity batches." OFF)
set(DURIN_UNITY_BUILD_BATCH_SIZE 8 CACHE STRING "Maximum source files in one Durin unity batch.")
if(NOT DURIN_UNITY_BUILD_BATCH_SIZE MATCHES "^[1-9][0-9]*$")
	message(FATAL_ERROR "DURIN_UNITY_BUILD_BATCH_SIZE must be a positive integer.")
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
			"ERROR: This IDE preset is code-model-only and cannot build. Use DevTool.bat with a registered build preset."
		COMMAND ${CMAKE_COMMAND} -E false
		VERBATIM
	)
	_durin_attach_code_model_build_guard("${CMAKE_SOURCE_DIR}" "${_durin_guard_target}")
endfunction()

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
