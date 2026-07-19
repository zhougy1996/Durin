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
