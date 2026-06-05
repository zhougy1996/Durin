# Common workspace-wide build options and compiler defaults.

include_guard(GLOBAL)

set(FETCHCONTENT_QUIET OFF)

if(NOT DEFINED ENABLE_DURIN_TIMER)
	set(ENABLE_DURIN_TIMER OFF)
endif()

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
