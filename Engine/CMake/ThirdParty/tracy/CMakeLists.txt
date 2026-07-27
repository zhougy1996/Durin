set(_durin_tracy_source_dir "${DURIN_WORKSPACE_DIR}/Engine/External/Source/tracy")
set(_durin_tracy_required_files
	"${_durin_tracy_source_dir}/CMakeLists.txt"
	"${_durin_tracy_source_dir}/LICENSE"
	"${_durin_tracy_source_dir}/public/TracyClient.cpp"
	"${_durin_tracy_source_dir}/public/tracy/Tracy.hpp"
)
foreach(_durin_tracy_required_file IN LISTS _durin_tracy_required_files)
	if(NOT EXISTS "${_durin_tracy_required_file}")
		message(FATAL_ERROR
			"Tracy profiling was requested, but its pinned source is missing or incomplete. "
			"Run Engine\\Scripts\\Bootstrap\\Setup_tracy.bat and configure again."
		)
	endif()
endforeach()

set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
set(TRACY_NO_CALLSTACK ON CACHE BOOL "" FORCE)
set(TRACY_NO_SAMPLING ON CACHE BOOL "" FORCE)
set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL "" FORCE)
set(TRACY_STATIC OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS ON)
add_subdirectory("${_durin_tracy_source_dir}" "${CMAKE_BINARY_DIR}/ThirdParty/tracy")

# Tracy v0.13.1 leaves this on-demand reconnect barrier set forever when
# callstack support is disabled because the absent symbol worker is its only
# normal reset path. Patch the build-local translation unit so prepared upstream
# source remains immutable and a later profiler can reconnect to the same client.
set(_durin_tracy_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/Generated")
set(_durin_tracy_profiler_source
	"${_durin_tracy_source_dir}/public/client/TracyProfiler.cpp"
)
set(_durin_tracy_client_source
	"${_durin_tracy_source_dir}/public/TracyClient.cpp"
)
set(_durin_tracy_patched_profiler
	"${_durin_tracy_generated_dir}/TracyProfiler.cpp"
)
set(_durin_tracy_patched_client
	"${_durin_tracy_generated_dir}/TracyClient.cpp"
)
file(MAKE_DIRECTORY "${_durin_tracy_generated_dir}")
file(READ "${_durin_tracy_profiler_source}" _durin_tracy_profiler_contents)
set(_durin_tracy_reconnect_barrier
	"        while( m_symbolsBusy.load( std::memory_order_acquire ) ) { YieldThread(); }\n        m_symbolsBusy.store( true, std::memory_order_release );"
)
set(_durin_tracy_guarded_reconnect_barrier
	"#ifdef TRACY_HAS_CALLSTACK\n${_durin_tracy_reconnect_barrier}\n#endif"
)
string(FIND
	"${_durin_tracy_profiler_contents}"
	"${_durin_tracy_reconnect_barrier}"
	_durin_tracy_reconnect_barrier_offset
)
if(_durin_tracy_reconnect_barrier_offset EQUAL -1)
	message(FATAL_ERROR
		"Tracy v0.13.1 reconnect compatibility patch no longer matches "
		"public/client/TracyProfiler.cpp."
	)
endif()
string(REPLACE
	"${_durin_tracy_reconnect_barrier}"
	"${_durin_tracy_guarded_reconnect_barrier}"
	_durin_tracy_profiler_contents
	"${_durin_tracy_profiler_contents}"
)
file(CONFIGURE
	OUTPUT "${_durin_tracy_patched_profiler}"
	CONTENT "${_durin_tracy_profiler_contents}"
	@ONLY
	NEWLINE_STYLE LF
)

file(READ "${_durin_tracy_client_source}" _durin_tracy_client_contents)
string(REPLACE
	"#include \"client/TracyProfiler.cpp\""
	"#include \"${_durin_tracy_patched_profiler}\""
	_durin_tracy_client_contents
	"${_durin_tracy_client_contents}"
)
file(CONFIGURE
	OUTPUT "${_durin_tracy_patched_client}"
	CONTENT "${_durin_tracy_client_contents}"
	@ONLY
	NEWLINE_STYLE LF
)
set_property(TARGET TracyClient PROPERTY SOURCES "${_durin_tracy_patched_client}")
target_include_directories(TracyClient PRIVATE
	"${_durin_tracy_source_dir}/public"
	"${_durin_tracy_source_dir}/public/client"
)

set_target_properties(TracyClient PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY
		"${DURIN_WORKSPACE_DIR}/Engine/Binaries/${DURIN_TARGET_PLATFORM}/${DURIN_OUTPUT_CONFIG}/Runtime/${DURIN_RUNTIME_VARIANT}"
)
